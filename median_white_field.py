"""Fast approximate synthetic white field; only the background is downsampled.

Large requested patches are represented by about 15 samples per axis on an
area-reduced float image. Its median is interpolated to the original image
size, then the original dark-corrected pixels are divided by that background.
This removes overall attenuation and is not absolute transmission retrieval.
"""
from dataclasses import dataclass
from functools import lru_cache
import time

import numpy as np


class MedianCancelled(Exception):
    pass


@dataclass(frozen=True)
class MedianResult:
    image: np.ndarray
    backend: str
    elapsed_ms: float
    invalid_pixels: int


def median_plan(shape, patch_size):
    """Return reduced image shape and median footprint (both y, x)."""
    if len(shape) != 2 or min(shape) < 1 or int(patch_size) != patch_size or patch_size < 1:
        raise ValueError("Median correction needs a nonempty 2-D image and a positive integer patch size.")
    step = max(1, (int(patch_size) + 14)//15)
    reduced = tuple(max(1, (int(n) + step - 1)//step) for n in shape)
    kernel = tuple(max(1, min(2*m-1, int(patch_size*m/n + 0.5)))
                   for n, m in zip(shape, reduced))
    return reduced, kernel


@lru_cache(maxsize=1)
def _cuda_torch():
    try:
        import torch
        return torch if torch.cuda.is_available() else None
    except (ImportError, OSError, RuntimeError):
        return None


def _gpu_median(image, kernel, torch, check):
    """Exact median of the reduced float image, with bounded CUDA scratch space.

    Index reflection duplicates the boundary pixel, matching SciPy's reflect
    mode. kthvalue selects the upper median for even-sized footprints, also
    matching SciPy; torch.median would select the lower median.
    """
    ky, kx = kernel
    ny, nx = image.shape
    device_image = torch.from_numpy(np.ascontiguousarray(image)).to("cuda")
    def reflected(n, k):
        left, right = k//2, (k-1)//2
        indices = torch.arange(-left, n+right, device="cuda") % (2*n)
        return torch.where(indices < n, indices, 2*n-1-indices).long()
    padded = device_image.index_select(0, reflected(ny, ky)).index_select(1, reflected(nx, kx))
    output = torch.empty_like(device_image)
    # At most ~64 MiB of expanded float patches, plus selection scratch.
    rows_per_chunk = max(1, 16_000_000//max(1, nx*ky*kx))
    for row in range(0, ny, rows_per_chunk):
        check()
        stop = min(ny, row + rows_per_chunk)
        windows = padded[row:stop+ky-1].unfold(0, ky, 1).unfold(1, kx, 1)
        patches = windows.contiguous().view(stop-row, nx, ky*kx)
        values = patches.kthvalue(ky*kx//2 + 1, dim=-1).values
        output[row:stop] = values
        del patches, windows, values
    return output.cpu().numpy()


def correct_median_white(frame, patch_size=100, *, dark=None, dark_gain=None,
                         data_gain=None, device="auto", cancelled=lambda: False):
    """Return relative intensity, preserving source samples and signed contrast.

    CUDA is useful for larger reduced-grid jobs. GPU imports are lazy; an
    unavailable runtime or CUDA allocation failure falls back to SciPy.
    Denominators that are nonpositive or tiny relative to the field are masked
    to zero. No gain, intensity quantization, or contrast stretch is applied.
    """
    start = time.perf_counter()
    def check():
        if cancelled():
            raise MedianCancelled()
    check()
    original = np.asarray(frame)
    reduced_shape, kernel = median_plan(original.shape, patch_size)
    if device not in ("auto", "cpu", "cuda"):
        raise ValueError("Median device must be auto, cpu or cuda.")
    data = np.array(original, dtype=np.float32, order="C", copy=True)
    if not np.isfinite(data).all():
        raise ValueError("Median correction requires finite image pixels.")
    if dark is not None:
        dark = np.asarray(dark, dtype=np.float32)
        if dark.shape != data.shape or not np.isfinite(dark).all():
            raise ValueError("Dark field must be finite and match the image shape.")
        if dark_gain is not None and data_gain is not None:
            dark = dark*np.float32(10**((data_gain-dark_gain)/20))
        data -= dark
        if not np.isfinite(data).all():
            raise ValueError("Dark subtraction produced nonfinite pixels; check field gains.")
    check()
    import cv2
    small = (cv2.resize(data, (reduced_shape[1], reduced_shape[0]), interpolation=cv2.INTER_AREA)
             if reduced_shape != data.shape else data)
    use_gpu = (device == "cuda" or
               (device == "auto" and small.size >= 65_536 and kernel[0]*kernel[1] >= 9))
    torch = _cuda_torch() if use_gpu else None
    background = None
    backend = "cpu"
    if torch is not None:
        try:
            with torch.inference_mode():
                background = _gpu_median(small, kernel, torch, check)
            backend = "cuda"
        except (RuntimeError, OSError):
            check()
    if background is None:
        from scipy.ndimage import median_filter
        background = median_filter(small, size=kernel, mode="reflect")
    check()
    if background.shape != data.shape:
        background = cv2.resize(background, (data.shape[1], data.shape[0]), interpolation=cv2.INTER_LINEAR)
    scale = float(np.max(np.abs(background)))
    floor = max(np.finfo(np.float32).tiny, scale*1e-6)
    valid = background > floor
    output = np.zeros(data.shape, dtype=np.float32)
    np.divide(data, background, out=output, where=valid)
    check()
    return MedianResult(output, backend, (time.perf_counter()-start)*1000,
                        int(valid.size - np.count_nonzero(valid)))
