#pragma once

// Thin wrappers around pocketfft with numpy.fft semantics (rfft / irfft with
// zero-padding or truncation to nfft, irfft normalised by 1/n).

#include <complex>
#include <cstddef>
#include <vector>

namespace roomeq
{
using cplx = std::complex<double>;

std::size_t nextPow2 (std::size_t n);

std::vector<cplx> rfft (const std::vector<double>& x, std::size_t nfft);
std::vector<double> irfft (const std::vector<cplx>& X, std::size_t nfft);
std::vector<double> rfftFreqs (std::size_t nfft, double fs);
} // namespace roomeq
