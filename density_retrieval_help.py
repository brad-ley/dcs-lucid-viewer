"""Lightweight, nonmodal guide to the nonlinear solver's regularization."""
from functools import lru_cache

import numpy as np
import pyqtgraph as pg

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QDialog, QDialogButtonBox, QLabel, QTabWidget, QTextBrowser,
    QVBoxLayout, QWidget,
)


_OVERVIEW = """
<h2>Choosing Alpha TV and Tikhonov Alpha</h2>
<p>The nonlinear solver searches for a phase map whose Fresnel-propagated
intensity agrees with the corrected input transmission. Regularization adds
preferences for which phase maps to accept when noise or weakly measured
information makes that fit uncertain. Phase is then converted to thickness
and areal density using the selected material.</p>
<h3>What each control does</h3>
<p><b>Alpha TV</b> penalizes differences between neighboring phase pixels.
Increasing it usually makes uniform regions quieter, while allowing edges.
Too much can flatten small features, reduce jump heights, and turn smooth
ramps into steps.</p>
<p><b>Tikhonov Alpha</b> penalizes the magnitude of the phase itself in this
viewer. Increasing it stabilizes weakly measured components, but also pulls
phase toward zero. It can reduce the mean thickness and areal density,
even in a broad, uniform region.</p>
<p>Neither parameter is a blur radius in pixels or microns. Their numerical
values are not directly comparable, and a cleaner image does not by itself
establish better spatial resolution or more accurate density.</p>
<h3>A practical tuning sequence</h3>
<ol>
<li>Check transmission normalization, material, energy, geometry and pixel
calibration first. Use a fixed image and processing ROI while comparing settings.</li>
<li>Start with the viewer defaults: <b>Alpha TV = 0.0001</b> and
<b>Tikhonov Alpha = 0.001</b>. These are starting values, not calibrated
choices for every material or image.</li>
<li>Change one parameter at a time, initially by factors of 2 to 10. Raise
Alpha TV if uniform areas remain noisy; lower it if thin features disappear
or smooth changes become flat steps.</li>
<li>Use enough Tikhonov regularization to stabilize the reconstruction, and
check whether increasing it changes the mean density or removes real contrast.
Compare regional means and line profiles, not just display smoothness.</li>
<li>Allow enough iterations and check whether a larger iteration limit changes
the result before attributing every difference to regularization. Repeat a
small parameter sweep around the useful range.</li>
</ol>
<p>Setting either alpha to zero disables that penalty; the other remains
active. Removing both can amplify noise or expose ambiguities in a
single-distance reconstruction. Revisit the settings after changing sampling,
ROI, exposure/noise level or material.</p>
<p>The other tabs show low, intermediate and high alpha values on the same
noisy, one-dimensional deblurring problem. Each parameter is varied alone.
These are calculated toy reconstructions, not your active image or a
measurement of the viewer's resolution.</p>
"""

_TV = """
<h2>Alpha TV: neighboring-pixel changes</h2>
<p><b>TV</b> means total variation. HoToPy sums a smoothed absolute value of the
horizontal and vertical phase differences. Alpha TV multiplies this sum.
A constant phase region has no difference-dependent TV cost, whatever its
absolute phase value.</p>
<p><b>Increase:</b> discourage small oscillations and noise; favor relatively
flat regions separated by edges. <b>Decrease:</b> retain more fine variation,
with a greater risk of noise and unstable texture. Strong TV can also bias
feature heights and create a staircase appearance on gradual slopes.</p>
<p><b>Example below:</b> a known block with a narrow raised feature is blurred
and noise is added. Too little TV lets the reconstruction fit noise. An
intermediate value reduces noise while retaining the feature. Too much TV
suppresses that feature and biases the block height.</p>
<p>Only TV is active in this example; Tikhonov Alpha is zero. The toy objective
is half the squared data mismatch plus Alpha TV times the smoothed total
variation, with smoothing constant 0.001 as in HoToPy.</p>
"""

_TIKHONOV = """
<h2>Tikhonov Alpha: phase magnitude</h2>
<p>This viewer passes a <b>single scalar</b> alpha to HoToPy. That applies a
constant weight at every Fourier frequency, including the mean (zero
frequency). By Parseval's identity, the resulting penalty is equivalent to a
squared phase-magnitude penalty in real space. It is not a squared-gradient
smoother or a Gaussian blur.</p>
<p><b>Increase:</b> damp poorly constrained components and stabilize the fit,
while pulling phase, thickness and areal density toward zero.
<b>Decrease:</b> let the data determine larger amplitudes, with more risk of
noise or artifacts where the measurement is weakly sensitive.</p>
<p>In the HoToPy 0.25 implementation used here, scalar alpha <b>a</b> gives a
penalty <b>2 a² sum(phase²)</b>.
Doubling Tikhonov Alpha therefore quadruples this penalty for the same phase
map. Alpha also regularizes the solver's initial CTF estimate.</p>
<p><b>Example below:</b> the same blurred, noisy block is reconstructed with
TV disabled. Too little Tikhonov regularization amplifies noise; an intermediate
value stabilizes the profile; too much shrinks the entire profile toward zero,
including its mean. The toy objective uses half the squared data mismatch plus
<b>2 a² sum(profile²)</b>, matching the scalar Tikhonov penalty above.</p>
"""


@lru_cache(maxsize=1)
def _regularization_example():
    """Solve a small deterministic inverse problem, never the active image.

    Both examples use y = A*x + noise, where A is periodic Gaussian blur.
    TV uses the same smoothed absolute differences as HoToPy. The scalar
    Tikhonov penalty is 2*a**2*||x||**2, giving a closed-form Fourier solution.
    Cache the six solutions so reopening help does not repeat the calculation.
    """
    from scipy.optimize import minimize

    n = 160
    position = np.linspace(0, 1, n, endpoint=False)
    truth = (0.12 + 0.8 * ((position >= 0.22) & (position < 0.78))
             + 0.3 * ((position >= 0.51) & (position < 0.57)))
    transfer = np.exp(-2 * np.pi**2 * 1.5**2 * np.fft.rfftfreq(n)**2)

    def blur(profile):
        return np.fft.irfft(transfer * np.fft.rfft(profile), n=n)

    measured = blur(truth) + np.random.default_rng(421).normal(0, 0.025, n)
    solutions = {"tv": [], "tikhonov": []}
    for alpha in (0.0001, 0.06, 1.2):
        def objective(profile):
            residual = blur(profile) - measured
            differences = np.diff(profile)
            smooth = np.sqrt(differences**2 + 0.001**2)
            loss = 0.5 * np.dot(residual, residual) + alpha * np.sum(smooth)
            gradient = blur(residual)
            flux = alpha * differences / smooth
            gradient[:-1] -= flux
            gradient[1:] += flux
            return loss, gradient

        fit = minimize(objective, measured, method="L-BFGS-B", jac=True,
                       options={"maxiter": 2000, "ftol": 1e-12, "gtol": 1e-7})
        if not fit.success:
            raise RuntimeError("The TV help example did not converge: " + fit.message)
        solutions["tv"].append((alpha, fit.x))

    spectrum = np.fft.rfft(measured)
    for alpha in (0.01, 0.06, 0.6):
        profile = np.fft.irfft(
            transfer * spectrum / (transfer**2 + 4 * alpha**2), n=n)
        solutions["tikhonov"].append((alpha, profile))
    return position, truth, measured, solutions


class IterativeSolverHelp(QDialog):
    """Explain the installed solver without importing torch or running retrieval."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Iterative Solver — Parameter Help")
        self.setWindowModality(Qt.WindowModality.NonModal)
        self.resize(880, 760)
        layout = QVBoxLayout(self)
        tabs = QTabWidget()
        layout.addWidget(tabs)
        overview = self._text(_OVERVIEW)
        tabs.addTab(overview, "Start here")
        tabs.addTab(self._parameter_tab(_TV, "tv"), "Alpha TV")
        tabs.addTab(self._parameter_tab(_TIKHONOV, "tikhonov"), "Tikhonov Alpha")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.close)
        layout.addWidget(buttons)

    @staticmethod
    def _text(html):
        text = QTextBrowser()
        text.setHtml(html)
        text.setMinimumHeight(180)
        text.document().setDocumentMargin(14)
        return text

    def _parameter_tab(self, html, kind):
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.addWidget(self._text(html), 1)
        position, truth, measured, solutions = _regularization_example()
        examples = solutions[kind]
        figure = pg.GraphicsLayoutWidget()
        figure.setBackground("#20242b")
        figure.setMinimumHeight(285)
        profiles = np.stack([truth, measured] + [profile for _, profile in examples])
        low, high = float(profiles.min()), float(profiles.max())
        margin = 0.08 * (high - low)
        first_plot = None
        for column, (label, (alpha, profile)) in enumerate(zip(
                ("Too low", "Intermediate", "Too high"), examples)):
            plot = figure.addPlot(row=0, col=column,
                                  title=f"{label}: α = {alpha:g}")
            plot.setMouseEnabled(x=False, y=False)
            plot.setMenuEnabled(False)
            plot.hideButtons()
            plot.showGrid(x=True, y=True, alpha=0.12)
            plot.setLabel("bottom", "Position", units="a.u.")
            if first_plot is None:
                first_plot = plot
                plot.setLabel("left", "Object profile", units="a.u.")
            else:
                plot.setYLink(first_plot)
                plot.getAxis("left").setStyle(showValues=False)
            plot.plot(position, measured, pen=None, symbol="o", symbolSize=3,
                      symbolPen=None, symbolBrush=(145, 155, 165, 150))
            plot.plot(position, truth,
                      pen=pg.mkPen("#eeeeee", width=1.5,
                                   style=Qt.PenStyle.DashLine), antialias=True)
            plot.plot(position, profile, pen=pg.mkPen("#5ac8fa", width=2),
                      antialias=True)
            plot.setXRange(0, 1, padding=0)
        first_plot.setYRange(min(0, low) - margin, high + margin, padding=0)
        layout.addWidget(figure, 1)
        key = QLabel(
            '<span style="color:#5ac8fa">Blue solid: reconstruction</span> '
            '&nbsp;&nbsp; <span style="color:#eeeeee">White dashed: true profile</span> '
            '&nbsp;&nbsp; Gray dots: noisy blurred data')
        key.setWordWrap(True)
        layout.addWidget(key)
        caption = QLabel(
            "Same known object and noisy data in all panels; the vertical scale is shared. "
            "Toy model: Gaussian blur (σ = 1.5 samples) + noise (σ = 0.025). "
            "These normalized alpha values are illustrative, not recommended PCI settings.")
        caption.setWordWrap(True)
        layout.addWidget(caption)
        return tab
