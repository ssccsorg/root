// tagma_compress: writes the block-compressed form of a plain store, with the
// block size as a parameter, so the scan-against-scatter tradeoff the block
// size sets can be measured rather than assumed.
//
//   root -l -b -q 'tagma_compress.C("/path/plain.bin", "/path/plain.z.bin", 18)'

#include "ROOT/TTagmaBlockSource.hxx"

#include <chrono>
#include <cstdio>
#include <string>

void tagma_compress(const char *in, const char *out, Int_t blockShift = 18)
{
   const auto start = std::chrono::steady_clock::now();
   std::string why;
   const bool ok = ROOT::TTagmaBlockSource::Compress(in, out, &why, static_cast<std::uint32_t>(blockShift));
   const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
   std::printf("tagma_compress: ok=%d wall=%.3f why=%s\n", ok ? 1 : 0, seconds, why.c_str());

   ROOT::TTagmaBlockSource source;
   if (source.Open(out, &why))
      std::printf("tagma_compress: payload=%llu file=%llu blocks=%llu blockBytes=%llu ratio=%.3f\n",
                  static_cast<unsigned long long>(source.PayloadBytes()),
                  static_cast<unsigned long long>(source.FileBytes()),
                  static_cast<unsigned long long>(source.BlockCount()),
                  static_cast<unsigned long long>(source.BlockBytes()),
                  static_cast<double>(source.PayloadBytes()) / static_cast<double>(source.FileBytes()));
   else
      std::printf("tagma_compress: open failed: %s\n", why.c_str());
}
