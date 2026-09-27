"""Python prototype of the Adaptive Room EQ Phase 1 analysis engine.

Modules mirror the planned C++ port:
    sweep      log sweep generation, deconvolution, arrival detection
    spectrum   IR windowing, fractional-octave smoothing, octave bands
    grading    per-band SNR / consistency grading with reasons
    averaging  multi-position power average, usable range, quick mode
    dualfft    program-material transfer function + coherence
    capture    one position's captures -> graded response
    roomsim    offline room + PA + noise simulator (pyroomacoustics)
"""
