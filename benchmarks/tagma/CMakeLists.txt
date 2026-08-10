# Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

# M5 benchmark (ssccs #103): the same logical workload, read every event
# of a dataset, through the pre-replacement baseline and through the
# coordinate-indexed store. The tagma_bench macro runs headless in
# synthetic mode so that ctest exercises both read paths without the
# multi-gigabyte remote dataset. The real-data runbook lives in
# README.md.

if(testing AND NOT WIN32)
  set(ROOT_root_CMD $<TARGET_FILE:root.exe>)

  ROOT_ADD_TEST(tagma-bench
    COMMAND ${ROOT_root_CMD} -b -q
            "${CMAKE_CURRENT_SOURCE_DIR}/tagma_bench.C(\"\", \"Events\", 20000, 2560, 3, 1)"
    PASSRC 0
    PASSREGEX "tagma_bench: comparison complete"
    LABELS benchmark
    TIMEOUT 600)
endif()
