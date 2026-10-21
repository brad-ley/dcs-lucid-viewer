"""Lightweight availability checks; safe to import without retrieval packages."""
import importlib.util
import sys


def missing_retrieval_dependencies(nonlinear=False):
    """Probe package metadata without importing any scientific/native modules."""
    packages = ('scipy', 'xraylib') + (('torch', 'hotopy') if nonlinear else ())
    missing = []
    for package in packages:
        try:
            available = importlib.util.find_spec(package) is not None
        except (ImportError, OSError, ValueError):
            available = False
        if not available:
            missing.append(package)
    return missing


def retrieval_unavailable_message(missing=(), error=None, nonlinear=False):
    tool = 'Nonlinear density retrieval' if nonlinear else 'Density Retrieval'
    message = f'{tool} is unavailable in this Python environment.'
    if missing:
        message += '\n\nMissing packages: ' + ', '.join(missing) + '.'
    if error is not None:
        message += f'\n\nCould not load the retrieval tool:\n{error}'
    if nonlinear:
        message += '\n\nThe Paganin Fourier filter remains available.'
    message += ('\n\nUse a compatible retrieval environment, or install the missing '
                'packages in the environment used to launch the viewer. '
                'The main viewer and measurement tools remain available.'
                f'\n\nActive Python: {sys.executable}')
    return message
