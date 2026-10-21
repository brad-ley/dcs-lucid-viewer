"""Single-distance density retrieval, matching pci_retrieval_gui.py.

Input is an already corrected 2-D transmission image (vacuum = 1). No dark,
flat-field, gain, contrast, or other intensity normalization is applied here.
Pixel pitch follows the reference GUI's sample-plane convention.
"""
from dataclasses import dataclass
import time

import numpy as np
from scipy.constants import h, c, e


@dataclass(frozen=True)
class RetrievalParameters:
    energy_keV: float = 22.3
    material: str = "Bi"
    density_g_cm3: float = 9.8
    rr1_m: float = 43.0
    rr2_m: float = 3.0
    pixel_size_um: float = 0.274
    solver: str = "paganin"
    alpha_tv: float = 1e-4
    alpha_tik: float = 1e-3
    max_iter: int = 50
    rtol: float = 1e-3
    device: str = "cpu"


@dataclass(frozen=True)
class RetrievalResult:
    thickness_m: np.ndarray
    elapsed_ms: float
    phase_per_m: float
    mu_per_m: float
    device: str = ""

    @property
    def exit_phase_rad(self):
        """Unwrapped projected phase relative to vacuum; negative for positive thickness."""
        return self.thickness_m * self.phase_per_m

    @property
    def exit_transmission(self):
        """Object-plane intensity ratio, not field amplitude or propagated intensity."""
        return np.exp(-self.mu_per_m * self.thickness_m)


class RetrievalCancelled(Exception):
    """The caller no longer needs this calculation."""


def retrieve_density(intensity, parameters, cancelled=lambda: False):
    """Return equivalent material thickness; multiply by rho * 1e5 for mg/cm².

    The caller owns the image snapshot. Inputs are never modified. Cancellation
    is checked between numerical stages; HoToPy's solver itself is not forcibly
    interrupted, so its thread must stay alive until the call returns.
    """
    def check_cancelled():
        if cancelled():
            raise RetrievalCancelled()

    check_cancelled()
    p = parameters
    image = np.asarray(intensity, dtype=np.float32)
    if image.ndim != 2 or not image.size or not np.isfinite(image).all():
        raise ValueError("Density retrieval needs a finite, nonempty 2-D image.")
    positive = (p.energy_keV, p.density_g_cm3, p.rr1_m, p.rr2_m,
                p.pixel_size_um, p.rtol)
    if not all(np.isfinite(value) and value > 0 for value in positive):
        raise ValueError("Energy, density, distances, pixel size and tolerance must be positive.")
    if not p.material.strip():
        raise ValueError("Enter a material formula.")
    if p.solver not in ("paganin", "nonlinear"):
        raise ValueError("Unknown retrieval solver.")
    if (not np.isfinite(p.alpha_tv) or p.alpha_tv < 0 or
            not np.isfinite(p.alpha_tik) or p.alpha_tik < 0 or p.max_iter < 1):
        raise ValueError("Regularization must be nonnegative and iterations must be positive.")
    if p.device not in ("cuda", "cpu"):
        raise ValueError("Solver device must be cuda or cpu.")
    try:
        import xraylib
    except ImportError as exc:
        raise RuntimeError("Density Retrieval requires xraylib. Install in the viewer "
                           "environment: python -m pip install xraylib") from exc

    start = time.perf_counter()
    wavelength = h * c / (p.energy_keV * 1e3 * e)
    delta = 1.0 - xraylib.Refractive_Index_Re(
        p.material.strip(), p.energy_keV, p.density_g_cm3)
    beta = xraylib.Refractive_Index_Im(
        p.material.strip(), p.energy_keV, p.density_g_cm3)
    if not np.isfinite(delta) or not np.isfinite(beta) or delta <= 0 or beta <= 0:
        raise ValueError("The material must have positive refractive-index delta and beta.")
    mu = 4 * np.pi * beta / wavelength
    magnification = (p.rr1_m + p.rr2_m) / p.rr1_m
    zeff = p.rr2_m / magnification
    dx = p.pixel_size_um * 1e-6
    check_cancelled()
    device = ""
    if p.solver == "nonlinear":
        try:
            import torch
            from hotopy.holo import TikhonovTV
        except ImportError as exc:
            raise RuntimeError("The nonlinear solver requires torch and hotopy. Install in "
                               "the viewer environment: python -m pip install torch hotopy") from exc
        check_cancelled()
        device = "cuda" if p.device == "cuda" and torch.cuda.is_available() else "cpu"
        solver = TikhonovTV(
            shape=image.shape, fresnel_nums=dx**2 / (wavelength * zeff),
            betadelta=beta / delta, alpha_tv=p.alpha_tv, alpha=p.alpha_tik,
            device=torch.device(device))
        tensor = torch.from_numpy(np.array(image, copy=True)).to(device)
        phase = solver(tensor, max_iter=p.max_iter, rtol=p.rtol)
        check_cancelled()
        thickness = -phase.detach().cpu().numpy() * wavelength / (2 * np.pi * delta)
    else:
        ny, nx = image.shape
        # The real FFT is equivalent to the reference's full complex FFT and
        # avoids storing the redundant half of a full-sensor Fourier image.
        fx = np.fft.rfftfreq(nx, d=dx)
        fy = np.fft.fftfreq(ny, d=dx)
        k_perp2 = (2 * np.pi * fy[:, None])**2 + (2 * np.pi * fx[None, :])**2
        denominator = 1.0 + zeff * delta / mu * k_perp2
        spectrum = np.fft.rfft2(image)
        check_cancelled()
        spectrum /= denominator
        filtered = np.fft.irfft2(spectrum, s=image.shape)
        # Match the reference: floor only after propagation filtering, before
        # taking log. Preserve negative input pixels and negative thickness.
        filtered[filtered <= 0] = 1e-10
        thickness = -np.log(filtered) / mu
    check_cancelled()
    if not np.isfinite(thickness).all():
        raise ValueError("Retrieval produced nonfinite values; check the image and parameters.")
    # Keep scalar optical coefficients from this calculation's parameter
    # snapshot. Alternate displays need no solver rerun or extra stored maps.
    return RetrievalResult(thickness, (time.perf_counter() - start) * 1000,
                           phase_per_m=-2 * np.pi * delta / wavelength,
                           mu_per_m=mu, device=device)

