// Author: SSCCS Foundation 2026

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TTagmaStore.hxx"

#include <limits>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ROOT {

namespace {

// File size in bytes, or a value that fails every range check when the
// file cannot be stat'ed.
std::uint64_t FileSize(const char *path)
{
#if defined(__unix__) || defined(__APPLE__)
   struct stat st;
   if (::stat(path, &st) != 0)
      return 0;
   return static_cast<std::uint64_t>(st.st_size);
#else
   (void)path;
   return 0;
#endif
}

}  // namespace

TTagmaStore::~TTagmaStore()
{
   Unmap();
}

TTagmaStore::TTagmaStore(TTagmaStore &&other) noexcept
    : fLayout(other.fLayout), fMap(other.fMap), fMapLen(other.fMapLen)
{
   other.fMap = nullptr;
   other.fMapLen = 0;
}

TTagmaStore &TTagmaStore::operator=(TTagmaStore &&other) noexcept
{
   if (this != &other) {
      Unmap();
      fLayout = other.fLayout;
      fMap = other.fMap;
      fMapLen = other.fMapLen;
      other.fMap = nullptr;
      other.fMapLen = 0;
   }
   return *this;
}

TTagmaStore::TTagmaStore(const Layout &layout) : fLayout(layout)
{
   if (fLayout.fRunMax == 0 || fLayout.fLumiMax == 0 ||
       fLayout.fEventMax == 0 || fLayout.fRecordSize == 0) {
      throw std::invalid_argument(
         "TTagmaStore: layout axes and record size must be nonzero");
   }
   if (fLayout.fRunMax > std::numeric_limits<std::uint64_t>::max() /
                            fLayout.fLumiMax) {
      throw std::invalid_argument("TTagmaStore: run_max * lumi_max overflows");
   }
   const std::uint64_t plane = fLayout.fRunMax * fLayout.fLumiMax;
   if (plane > std::numeric_limits<std::uint64_t>::max() /
                   fLayout.fEventMax) {
      throw std::invalid_argument("TTagmaStore: record count overflows");
   }
   const std::uint64_t count = plane * fLayout.fEventMax;
   if (count > std::numeric_limits<std::uint64_t>::max() /
                   fLayout.fRecordSize) {
      throw std::invalid_argument("TTagmaStore: store extent overflows");
   }
}

std::uint64_t TTagmaStore::Compose(std::uint64_t run, std::uint64_t lumi,
                                   std::uint64_t event) const
{
   return (run * fLayout.fLumiMax + lumi) * fLayout.fEventMax + event;
}

std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> TTagmaStore::Decompose(
    std::uint64_t index) const
{
   const std::uint64_t event = index % fLayout.fEventMax;
   const std::uint64_t rest = index / fLayout.fEventMax;
   const std::uint64_t lumi = rest % fLayout.fLumiMax;
   const std::uint64_t run = rest / fLayout.fLumiMax;
   return {run, lumi, event};
}

std::uint64_t TTagmaStore::Offset(std::uint64_t run, std::uint64_t lumi,
                                  std::uint64_t event) const
{
   return Compose(run, lumi, event) * fLayout.fRecordSize;
}

std::uint64_t TTagmaStore::RecordCount() const
{
   return fLayout.fRunMax * fLayout.fLumiMax * fLayout.fEventMax;
}

std::uint64_t TTagmaStore::SizeBytes() const
{
   return RecordCount() * fLayout.fRecordSize;
}

bool TTagmaStore::Contains(std::uint64_t run, std::uint64_t lumi,
                           std::uint64_t event) const
{
   return run < fLayout.fRunMax && lumi < fLayout.fLumiMax &&
          event < fLayout.fEventMax;
}

bool TTagmaStore::MapFile(const char *path)
{
   Unmap();
#if defined(__unix__) || defined(__APPLE__)
   if (path == nullptr || path[0] == '\0')
      return false;
   const std::uint64_t need = SizeBytes();
   // The mapping length must fit the platform address size.
   if (need > std::numeric_limits<std::size_t>::max())
      return false;
   const std::uint64_t size = FileSize(path);
   if (size < need)
      return false;
   const int fd = ::open(path, O_RDONLY);
   if (fd < 0)
      return false;
   void *ptr = ::mmap(nullptr, static_cast<std::size_t>(need), PROT_READ,
                      MAP_PRIVATE, fd, 0);
   ::close(fd);
   if (ptr == MAP_FAILED)
      return false;
   fMap = ptr;
   fMapLen = static_cast<std::size_t>(need);
   return true;
#else
   (void)path;
   return false;
#endif
}

void TTagmaStore::Unmap()
{
#if defined(__unix__) || defined(__APPLE__)
   if (fMap != nullptr) {
      ::munmap(fMap, fMapLen);
      fMap = nullptr;
      fMapLen = 0;
   }
#else
   fMap = nullptr;
   fMapLen = 0;
#endif
}

}  // namespace ROOT
