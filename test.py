import sys
import numpy as np
import matplotlib.pyplot as plt

# 1. Define physical constants and parameters
wavelength = 500e-9  # 500 nm (Green light)
k = 2 * np.pi / wavelength  # Wave number
z = 0.1  # Distance from aperture to screen (10 cm)
screen_size = 0.01  # 1 cm screen width

# 2. Define the Aperture Plane (Source)
slit_width = 1e-5         # 10 um width of each slit (x-direction)
slit_separation = 2.5e-4  # center-to-center distance between the two slits

# Only sample where the field is actually nonzero (inside the two slits), instead of the
# full 1 cm screen width. That also fixes under-resolution: a 500-pt grid across 1 cm has
# dx ~ 20 um, coarser than the 10 um slit, so the mask-based version couldn't resolve it.
n_x_per_slit = 15  # x-samples across each slit (resolves the slit width)

x_slit1 = np.linspace(-slit_separation/2 - slit_width/2, -slit_separation/2 + slit_width/2, n_x_per_slit)
x_slit2 = np.linspace(slit_separation/2 - slit_width/2, slit_separation/2 + slit_width/2, n_x_per_slit)
x_ap = np.concatenate([x_slit1, x_slit2])
dx = x_slit1[1] - x_slit1[0]

# 3. Define the Observation Plane (Screen)
# The slits are treated as effectively infinite/uniform in y, so the field on the screen
# doesn't depend on y_sc. Integrating that dimension numerically (as the previous version
# did, over 200 samples spread across the full 1 cm aperture) requires the phase k*r to
# change by far less than 2*pi between adjacent samples to avoid aliasing; with such a
# wide, coarsely-sampled y range that condition was badly violated, producing the fake
# horizontal banding. Since the true answer has no y-dependence, we skip that integral
# entirely: compute the 1D Fresnel-Kirchhoff pattern along x once, then tile it vertically.
x_sc = np.linspace(-screen_size/2, screen_size/2, 200)
y_sc = np.linspace(-screen_size/2, screen_size/2, 200)

# 4. Compute the 1D Fresnel-Kirchhoff Integral (Discrete Summation) along x
E_line = np.zeros_like(x_sc, dtype=complex)

print("Computing diffraction pattern...")
total = len(x_sc)
for i, x in enumerate(x_sc):
    # Distance r from all points on the aperture to this point on the screen
    r = np.sqrt((x - x_ap)**2 + z**2)

    # Kirchhoff obliquity factor (cos(theta) approx z/r)
    obliquity = (z / r + 1) / 2

    # Fresnel-Kirchhoff integrand: (E0 / r) * exp(i*k*r) * obliquity
    integrand = (1.0 / r) * np.exp(1j * k * r) * obliquity

    # Perform the line integration (summing over the aperture)
    E_line[i] = (-1j / wavelength) * np.sum(integrand) * dx

    # Progress bar
    frac = (i + 1) / total
    bar_len = 40
    filled = int(bar_len * frac)
    bar = '#' * filled + '-' * (bar_len - filled)
    sys.stdout.write(f'\r[{bar}] {frac * 100:5.1f}% ({i + 1}/{total})')
    sys.stdout.flush()
print()

# 5. Calculate Intensity (Proportional to the square of the magnitude), tiled across y
intensity_1d = np.abs(E_line)**2
intensity_1d /= np.max(intensity_1d)  # Normalize
intensity = np.tile(intensity_1d, (len(y_sc), 1))

# 6. Plot the results
X_sc, Y_sc = np.meshgrid(x_sc, y_sc)
plt.figure(figsize=(6, 5))
plt.pcolormesh(X_sc * 1000, Y_sc * 1000, intensity, cmap='inferno', shading='auto')
plt.colorbar(label='Normalized Intensity')
plt.xlabel('X position on screen (mm)')
plt.ylabel('Y position on screen (mm)')
plt.title('Fresnel-Kirchhoff Diffraction Pattern')
plt.show()
