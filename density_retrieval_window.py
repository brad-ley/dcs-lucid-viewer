"""Nonmodal PyQt6 Density Retrieval tool for the active viewer image."""
from viewer_runtime import prepare_qt_runtime

prepare_qt_runtime()

import numpy as np
import pyqtgraph as pg

from PyQt6.QtCore import Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QAction, QActionGroup, QTransform
from PyQt6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QFormLayout, QGroupBox, QLabel,
    QDoubleSpinBox, QSpinBox, QLineEdit, QCheckBox, QPushButton, QComboBox,
    QScrollArea, QSplitter, QProgressBar, QToolButton, QSizePolicy,
)

from retrieval_roi import ProcessingROI, ROISelectionViewBox

from density_retrieval import (
    RetrievalParameters, RetrievalCancelled, retrieve_density,
)


class _PixelSizeInput(QDoubleSpinBox):
    def textFromValue(self, value):
        text = super().textFromValue(value)
        # Keep calculation precision without a row of insignificant trailing zeros.
        point = self.locale().decimalPoint()
        return text.rstrip('0').rstrip(point) if point in text else text


class _RetrievalWorker(QThread):
    result_ready = pyqtSignal(int, object, object, object)
    failed = pyqtSignal(int, str)

    def __init__(self, frame, parameters, revision, parent, bounds=None):
        super().__init__(parent)
        self.frame = frame
        self.parameters = parameters
        self.revision = revision
        self.bounds = bounds or (0, 0, frame.shape[1], frame.shape[0])

    def run(self):
        try:
            result = retrieve_density(self.frame, self.parameters,
                                      cancelled=self.isInterruptionRequested)
            self.result_ready.emit(self.revision, self.parameters, result, self.bounds)
        except RetrievalCancelled:
            pass
        except Exception as exc:
            self.failed.emit(self.revision, str(exc))


class DensityRetrievalWindow(QMainWindow):
    """Follow viewer frames, with at most one running and one pending request.

    Every calculation retains an independent corrected image and parameter
    snapshot. GUI changes invalidate older results. Closing during a solver
    call hides the window and defers destruction until its QThread finishes.
    """
    closed = pyqtSignal()

    def __init__(self, parent=None, stylesheet=""):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.setWindowTitle("Density Retrieval")
        self.resize(1350, 780)
        if stylesheet:
            self.setStyleSheet(stylesheet)
        self._frame = None
        self._source_key = None
        self._revision = 0
        self._worker = None
        self._requested = False
        self._closing = False
        self._closed = False
        self._result = None
        self._result_parameters = None
        self._result_bounds = None
        self.retrieved_map = None
        self._solver_help = None
        self._summary = ""
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.setInterval(150)
        self._debounce.timeout.connect(self._start_latest)
        self._build_ui()

    @property
    def is_busy(self):
        return self._worker is not None

    @property
    def is_closing(self):
        return self._closing

    @staticmethod
    def _double(form, label, low, high, decimals, value, step=None):
        spin = QDoubleSpinBox()
        spin.setRange(low, high)
        spin.setDecimals(decimals)
        spin.setValue(value)
        if step is not None:
            spin.setSingleStep(step)
        form.addRow(label, spin)
        return spin

    def _build_ui(self):
        solver_menu = self.menuBar().addMenu("Phase Retrieval")
        group = QActionGroup(self)
        self.paganin_action = QAction("Paganin Fourier filter", self, checkable=True)
        self.nonlin_action = QAction("Nonlinear Fresnel with TV (TikhonovTV)",
                                     self, checkable=True)
        for action in (self.paganin_action, self.nonlin_action):
            group.addAction(action)
            solver_menu.addAction(action)
        self.paganin_action.setChecked(True)
        self.paganin_action.triggered.connect(self._solver_changed)
        self.nonlin_action.triggered.connect(self._solver_changed)

        aspect_menu = self.menuBar().addMenu("Aspect Ratio")
        aspect_group = QActionGroup(self)
        equal = QAction("Equal", self, checkable=True)
        stretch = QAction("Auto (Stretch)", self, checkable=True)
        equal.setChecked(True)
        for action in (equal, stretch):
            aspect_group.addAction(action)
            aspect_menu.addAction(action)
        equal.triggered.connect(lambda: self._set_aspect(True))
        stretch.triggered.connect(lambda: self._set_aspect(False))

        splitter = QSplitter(Qt.Orientation.Horizontal)
        self.setCentralWidget(splitter)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setMinimumWidth(300)
        panel = QWidget()
        left = QVBoxLayout(panel)
        self.source_label = QLabel("No active image.")
        self.source_label.setWordWrap(True)
        self.source_label.setTextFormat(Qt.TextFormat.PlainText)
        left.addWidget(self.source_label)
        note = QLabel("Uses the active viewer image before display contrast. "
                      "Retrieval expects transmission normalized to 1 in vacuum.")
        note.setWordWrap(True)
        left.addWidget(note)

        physical = QGroupBox("Physical Parameters")
        form = QFormLayout(physical)
        self.energy_input = self._double(form, "Energy (keV):", 1, 200, 2, 22.3)
        self.mat_input = QLineEdit("Bi")
        form.addRow("Material Formula:", self.mat_input)
        self.rr1_input = self._double(form, "Source → sample, RR1 (m):",
                                     0.1, 1000, 1, 43)
        self.rr2_input = self._double(form, "Sample → detector, RR2 (m):",
                                     0.01, 20, 2, 3)
        self.pixel_input = _PixelSizeInput()
        self.pixel_input.setDecimals(15)
        self.pixel_input.setRange(1e-15, 1e15)
        self.pixel_input.setValue(0.274)
        self.pixel_input.setSingleStep(.001)
        self.pixel_input.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.pixel_input.setMinimumWidth(90)
        form.addRow("Pixel Size (µm):", self.pixel_input)
        self.pixel_input.setToolTip(
            "Sample-plane pixel pitch, using the reference retrieval GUI's convention. "
            "Initially taken from the main viewer's active scale. Tools → Set Scale… "
            "overrides sensor pitch / objective magnification.")
        left.addWidget(physical)

        self.roi_group = ProcessingROI()
        left.addWidget(self.roi_group)

        retrieval = QGroupBox("Retrieval")
        actions = QVBoxLayout(retrieval)
        self.chk_auto = QCheckBox("Auto Retrieve Phase")
        self.chk_auto.setChecked(True)
        self.btn_retrieve = QPushButton("Retrieve Phase")
        self.btn_retrieve.setEnabled(False)
        actions.addWidget(self.chk_auto)
        actions.addWidget(self.btn_retrieve)
        left.addWidget(retrieval)

        self.solver_group = QGroupBox("Iterative Solver")
        form = QFormLayout(self.solver_group)
        self.btn_solver_help = QToolButton()
        self.btn_solver_help.setText("?")
        self.btn_solver_help.setAccessibleName("Iterative solver parameter help")
        self.btn_solver_help.setToolTip("Explain Alpha TV and Tikhonov Alpha")
        self.btn_solver_help.clicked.connect(self._show_solver_help)
        form.addRow("Parameter help:", self.btn_solver_help)
        self.alpha_tv_input = self._double(form, "Alpha TV:", 0, 10, 6, 1e-4, 1e-5)
        self.alpha_tik_input = self._double(form, "Tikhonov Alpha:", 0, 10, 5, 1e-3, 1e-4)
        self.max_iter_input = QSpinBox()
        self.max_iter_input.setRange(1, 2000)
        self.max_iter_input.setSingleStep(10)
        self.max_iter_input.setValue(50)
        form.addRow("Max Iterations:", self.max_iter_input)
        self.rtol_input = self._double(form, "Tolerance (rtol):",
                                      1e-7, 1, 7, 1e-3, 1e-4)
        self.device_input = QComboBox()
        self.device_input.addItems(["cuda", "cpu"])
        self.device_input.setToolTip("CUDA is used if available; otherwise the solver uses CPU.")
        form.addRow("Solver Device:", self.device_input)
        self.solver_group.hide()
        left.addWidget(self.solver_group)

        output = QGroupBox("Output")
        form = QFormLayout(output)
        self.output_input = QComboBox()
        for label, key in (("Areal density (mg/cm²)", "areal_density"),
                           ("Thickness (µm)", "thickness"),
                           ("Exit phase (rad)", "exit_phase"),
                           ("Object-plane transmission (I/I₀)", "exit_transmission")):
            self.output_input.addItem(label, key)
        self.output_input.setToolTip("Change the retrieved display without rerunning the solver.")
        form.addRow("Plot:", self.output_input)
        self.density_input = self._double(form, "Density (g/cm³):", 0.01, 100, 2, 9.8)
        self.output_note = QLabel()
        self.output_note.setWordWrap(True)
        form.addRow(self.output_note)
        left.addWidget(output)
        left.addStretch()
        scroll.setWidget(panel)
        splitter.addWidget(scroll)

        plots = pg.GraphicsLayoutWidget()
        self.p1 = plots.addPlot(title="Input Transmission", viewBox=ROISelectionViewBox())
        self.img1 = pg.ImageItem(axisOrder="row-major")
        self.p1.addItem(self.img1)
        self.roi_group.attach(self.img1, self.p1.getViewBox())
        self.lut1 = pg.HistogramLUTItem(image=self.img1)
        self.lut1.gradient.loadPreset("grey")
        plots.addItem(self.lut1)
        self.p2 = plots.addPlot(title="Retrieved Areal Density (mg/cm²)")
        self.img2 = pg.ImageItem(axisOrder="row-major")
        self.p2.addItem(self.img2)
        self.lut2 = pg.HistogramLUTItem(image=self.img2)
        self.lut2.gradient.loadPreset("viridis")
        plots.addItem(self.lut2)
        for plot in (self.p1, self.p2):
            plot.setLabel("bottom", "x (mm)")
            plot.setLabel("left", "y (mm)")
            plot.invertY(True)
            plot.setAspectLocked(True, ratio=1)
        self.p2.setXLink(self.p1)
        self.p2.setYLink(self.p1)
        self._mouse_proxy = pg.SignalProxy(
            plots.scene().sigMouseMoved, rateLimit=30,
            slot=lambda args: self._mouse_moved(args[0]))
        splitter.addWidget(plots)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([320, 1030])

        self._progress = QProgressBar()
        self._progress.setRange(0, 0)
        self._progress.setMaximumWidth(120)
        self.statusBar().addPermanentWidget(self._progress)
        self._progress.hide()

        for spin in (self.energy_input, self.rr1_input, self.rr2_input,
                     self.pixel_input, self.alpha_tv_input, self.alpha_tik_input,
                     self.max_iter_input, self.rtol_input, self.density_input):
            spin.valueChanged.connect(self._parameters_changed)
        self.mat_input.editingFinished.connect(self._parameters_changed)
        self.device_input.currentTextChanged.connect(self._parameters_changed)
        self.output_input.currentIndexChanged.connect(self._display_result)
        self.chk_auto.toggled.connect(self._auto_changed)
        self.btn_retrieve.clicked.connect(self.run_retrieval)
        self.roi_group.region_changing.connect(self._invalidate)
        self.roi_group.region_changed.connect(self._roi_finished)
        self._display_result()

    def _show_solver_help(self):
        if self._closing:
            return
        if self._solver_help is None:
            from density_retrieval_help import IterativeSolverHelp
            self._solver_help = IterativeSolverHelp(self)
        self._solver_help.show()
        self._solver_help.raise_()
        self._solver_help.activateWindow()

    def parameters(self):
        return RetrievalParameters(
            energy_keV=self.energy_input.value(), material=self.mat_input.text(),
            density_g_cm3=self.density_input.value(), rr1_m=self.rr1_input.value(),
            rr2_m=self.rr2_input.value(), pixel_size_um=self.pixel_input.value(),
            solver="nonlinear" if self.nonlin_action.isChecked() else "paganin",
            alpha_tv=self.alpha_tv_input.value(), alpha_tik=self.alpha_tik_input.value(),
            max_iter=self.max_iter_input.value(), rtol=self.rtol_input.value(),
            device=self.device_input.currentText())

    def set_frame(self, frame, source_label, pixel_size_um=None, source_key=None):
        if self._closing:
            return
        if frame is None:
            self.clear_frame()
            return
        first = self._frame is None
        reset_roi = (first or source_key != self._source_key or
                     np.shape(frame) != self._frame.shape)
        self._frame = np.array(frame, dtype=np.float32, order="C", copy=True)
        self._frame.flags.writeable = False
        # Preserve a user's pixel-size edit while navigating the same recording.
        if source_key != self._source_key or first:
            calibrated = (pixel_size_um is not None and
                          np.isfinite(pixel_size_um) and pixel_size_um > 0)
            pitch = pixel_size_um if calibrated else RetrievalParameters().pixel_size_um
            self.pixel_input.blockSignals(True)
            self.pixel_input.setValue(pitch)
            self.pixel_input.blockSignals(False)
        self._source_key = source_key
        self.source_label.setText(source_label)
        self._invalidate()
        self.img1.setImage(self._frame, autoLevels=True)
        self._set_transform()
        self.roi_group.set_image_shape(self._frame.shape, reset=reset_roi)
        if first:
            self.p1.autoRange()
        self.btn_retrieve.setEnabled(not self.chk_auto.isChecked())
        if self.chk_auto.isChecked():
            self._schedule()

    def clear_frame(self):
        self._frame = None
        self._source_key = None
        self._invalidate()
        self.img1.clear()
        self.roi_group.set_image_shape(None)
        self.source_label.setText("No active image.")
        self.btn_retrieve.setEnabled(False)

    def _invalidate(self):
        self._revision += 1
        self._requested = False
        self._debounce.stop()
        self._result = None
        self._result_parameters = None
        self._result_bounds = None
        self.retrieved_map = None
        self.img2.clear()
        self._summary = "Image or parameters changed; retrieval pending."
        self.statusBar().showMessage(self._summary)
        if self._worker is not None:
            self._worker.requestInterruption()

    def _solver_changed(self, *_args):
        self.solver_group.setVisible(self.nonlin_action.isChecked())
        self._parameters_changed()

    def _parameters_changed(self, *_args):
        self._invalidate()
        self._set_transform()
        if self.chk_auto.isChecked() and self._frame is not None:
            self._schedule()

    def _auto_changed(self, checked):
        self.btn_retrieve.setEnabled(not checked and self._frame is not None)
        if checked and self._frame is not None:
            self._schedule()
        else:
            self._requested = False
            self._debounce.stop()

    def _roi_finished(self):
        if self.chk_auto.isChecked() and self._frame is not None:
            self._schedule()

    def _schedule(self):
        self._requested = True
        if not self.is_busy:
            self._debounce.start()

    def run_retrieval(self, *_args):
        if self._frame is None or self._closing:
            return
        self._requested = True
        self._debounce.stop()
        self._start_latest()

    def _start_latest(self):
        if self.is_busy or not self._requested or self._closing or self._frame is None:
            return
        self._requested = False
        bounds = self.roi_group.bounds()
        x, y, width, height = bounds
        frame = self._frame
        if self.roi_group.isChecked():
            # Snapshot only selected pixels. Both solvers see this smaller
            # array; excluded borders never enter FFTs or GPU allocations.
            frame = np.array(frame[y:y + height, x:x + width], order='C', copy=True)
            frame.flags.writeable = False
        self._worker = _RetrievalWorker(frame, self.parameters(), self._revision,
                                        self, bounds=bounds)
        self._worker.result_ready.connect(self._on_result)
        self._worker.failed.connect(self._on_error)
        self._worker.finished.connect(self._on_finished)
        self._progress.show()
        self._summary = f"Retrieving {width} × {height} px…"
        self.statusBar().showMessage(self._summary)
        self._worker.start()

    def _on_result(self, revision, parameters, result, bounds):
        if self._closing or revision != self._revision:
            return
        self._result = result
        self._result_parameters = parameters
        self._result_bounds = bounds
        self._display_result()
        name = "Nonlinear Fresnel" if parameters.solver == "nonlinear" else "Paganin"
        device = f" on {result.device}" if result.device else ""
        self._summary = f"{name} retrieval complete in {result.elapsed_ms:.1f} ms{device}"
        self.statusBar().showMessage(self._summary)

    def _on_error(self, revision, message):
        if not self._closing and revision == self._revision:
            self._summary = f"Retrieval failed: {message}"
            self.statusBar().showMessage(self._summary)

    def _on_finished(self):
        worker = self._worker
        self._worker = None
        worker.deleteLater()
        self._progress.hide()
        if self._closing:
            self.close()
        else:
            self._start_latest()

    def _display_result(self, *_args):
        output = self.output_input.currentData()
        title, self._output_unit, note = {
            "areal_density": ("Retrieved Areal Density (mg/cm²)", "mg/cm²",
                             "Equivalent areal density for the selected material."),
            "thickness": ("Retrieved Thickness (µm)", "µm",
                          "Equivalent thickness at the selected material density."),
            "exit_phase": ("Retrieved Exit Phase (rad)", "rad",
                           "Unwrapped phase relative to vacuum (0 rad). Positive material "
                           "thickness gives negative phase."),
            "exit_transmission": ("Retrieved Object-plane Transmission (I/I₀)", "I/I₀",
                                  "Intensity just after the object, before propagation: "
                                  "I_exit / I_incident (vacuum = 1)."),
        }[output]
        self.p2.setTitle(title)
        self.output_note.setText(note)
        # Clear a previous cursor readout, whose value and units may differ.
        self.statusBar().showMessage(self._summary)
        if self._result is None:
            return
        if output == "exit_phase":
            self.retrieved_map = self._result.exit_phase_rad
        elif output == "exit_transmission":
            self.retrieved_map = self._result.exit_transmission
        else:
            scale = (1e6 if output == "thickness" else
                     self._result_parameters.density_g_cm3 * 1e5)
            self.retrieved_map = self._result.thickness_m * scale
        self.img2.setImage(self.retrieved_map, autoLevels=True)
        self._set_transform()

    def _set_transform(self):
        dx_mm = self.pixel_input.value() * 1e-3
        self.img1.setTransform(QTransform(dx_mm, 0, 0, dx_mm, 0, 0))
        x, y = self._result_bounds[:2] if self._result_bounds else (0, 0)
        self.img2.setTransform(QTransform(dx_mm, 0, 0, dx_mm, x * dx_mm, y * dx_mm))

    def _set_aspect(self, equal):
        for plot in (self.p1, self.p2):
            plot.setAspectLocked(equal, ratio=1)

    def _mouse_moved(self, pos):
        if self._closing or self._frame is None:
            return
        for plot in (self.p1, self.p2):
            view = plot.getViewBox()
            if not view.sceneBoundingRect().contains(pos):
                continue
            point = view.mapSceneToView(pos)
            pitch = self.pixel_input.value() * 1e-3
            x, y = int(np.floor(point.x() / pitch)), int(np.floor(point.y() / pitch))
            if 0 <= y < self._frame.shape[0] and 0 <= x < self._frame.shape[1]:
                text = (f"X: {point.x():.4f} mm, Y: {point.y():.4f} mm"
                        f" | Input: {self._frame[y, x]:.4f}")
                if self.retrieved_map is not None:
                    x0, y0, width, height = self._result_bounds
                    if x0 <= x < x0 + width and y0 <= y < y0 + height:
                        text += (f" | Retrieved: {self.retrieved_map[y - y0, x - x0]:.4f} "
                                 f"{self._output_unit}")
                self.statusBar().showMessage(text)
                return
        self.statusBar().showMessage(self._summary)

    def closeEvent(self, event):
        if not self._closing:
            self._mouse_proxy.disconnect()
            if self._solver_help is not None:
                self._solver_help.close()
        self._closing = True
        self._requested = False
        self._debounce.stop()
        if self._worker is not None:
            self._worker.requestInterruption()
            self.hide()
            event.ignore()
            return
        if not self._closed:
            self._closed = True
            self.closed.emit()
        event.accept()

