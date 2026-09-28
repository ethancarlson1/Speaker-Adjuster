#include "roomeq/fft.h"

#include <pocketfft_hdronly.h>

#include <algorithm>

namespace roomeq
{
std::size_t nextPow2 (std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

std::vector<cplx> rfft (const std::vector<double>& x, std::size_t nfft)
{
    std::vector<double> in (nfft, 0.0);
    std::copy_n (x.begin(), std::min (x.size(), nfft), in.begin());
    std::vector<cplx> out (nfft / 2 + 1);
    pocketfft::r2c<double> ({ nfft }, { sizeof (double) }, { sizeof (cplx) }, 0, pocketfft::FORWARD,
                            in.data(), out.data(), 1.0);
    return out;
}

std::vector<double> irfft (const std::vector<cplx>& X, std::size_t nfft)
{
    std::vector<cplx> in (nfft / 2 + 1, cplx {});
    std::copy_n (X.begin(), std::min (X.size(), in.size()), in.begin());
    std::vector<double> out (nfft);
    pocketfft::c2r<double> ({ nfft }, { sizeof (cplx) }, { sizeof (double) }, 0, pocketfft::BACKWARD,
                            in.data(), out.data(), 1.0 / static_cast<double> (nfft));
    return out;
}

std::vector<double> rfftFreqs (std::size_t nfft, double fs)
{
    std::vector<double> f (nfft / 2 + 1);
    for (std::size_t k = 0; k < f.size(); ++k)
        f[k] = static_cast<double> (k) * fs / static_cast<double> (nfft);
    return f;
}
} // namespace roomeq
