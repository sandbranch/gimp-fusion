# Plan

## Done

Image > Merge Exposures, Focus Stack and Align Layers by Content (README):
a C plug-in on a GIMP-free C core that streams the stack (memory of a few
images whatever the number of layers). Exposure fusion as in Mertens'
code; four focus stacking methods and a depth map; alignment by phase
correlation and Gauss-Newton (shift, scale, rotation), chained through
neighbours. Tested with tests/run.sh: unit tests (also under ASan and
UBSan), cross-checks against a numpy port of Mertens' Matlab code and
OpenCV, synthetic scenes with known answers, the plug-in in GIMP 3.2.6
(Flatpak, also under ASan), real CC0 and NASA images, the dialogs on
Broadway, benchmarks.

## Next

1. **Ghost removal** for brackets with moving things (people, water,
   leaves): weights that drop pixels which disagree with the reference
   exposure after brightness matching.
2. **Faster in GIMP**: each layer is read two or three times, which costs
   about as much as the fusion. Keep the layers in memory as half floats
   when they fit (a memory setting), or read with larger requests.
3. **A cancel button** (progress with cancel) for long stacks.
4. **Alignment**: a homography model for handheld brackets; a check that
   refuses a transform that makes the match worse; an optional fit of the
   magnification to a straight line over the stack (focus breathing is
   linear in the focus position on MAHLI, and chaining errors would
   average out); a way to choose the reference layer in the dialogs.
5. **The defocus and magnification ambiguity** (README, Limits): compare
   at matched blur (blur the sharper image of a pair where it is
   sharper) before estimating scale.
6. **Microscopy stitching** (candidates.md): the phase correlation and
   refinement here are most of a translation-only mosaic tool.
7. **HDR radiance merge**: wire `gegl:exp-combine` (in GEGL, not in any
   GIMP menu) with exposure times from Exif, for tone mapping with GIMP's
   own operators.
8. Translations (the plug-in's set_i18n is off for now).
9. Offer the core to GEGL? Only once GIMP can keep filters with more than
   one input editable (GIMP issue #11904).

## Found along the way

- libgimp installs its own SIGSEGV handler in plug-ins ("fatal error:
  Segmentation fault"), which hides AddressSanitizer's report; with
  `ASAN_OPTIONS=allow_user_segv_handler=0` ASan keeps its handler and
  reports (tests/gui/start.sh, GUI_ASAN=1).
- `gimp_procedure_dialog_set_sensitive_if_in` takes the value array: the
  caller must not unref it (a crash when the dialog closes). Worth a
  "(transfer full)" note in the libgimp docs, to be checked there.
- `getrusage`'s `ru_maxrss` survives execve, so in a plug-in (forked from
  GIMP) it is GIMP's own peak; VmHWM in /proc/self/status is the
  plug-in's.
- `gimp_procedure_add_menu_path` before `gimp_procedure_set_menu_label`
  gives a critical and no menu entry.
- For choice arguments, GimpProcedureDialog takes GTK_TYPE_COMBO_BOX or
  GIMP_TYPE_INT_RADIO_FRAME; GIMP_TYPE_STRING_COMBO_BOX is ignored
  silently (the default widget is used). Radio frame titles cannot carry
  mnemonics, and GIMP then warns "no mnemonic for property".
- babl may convert the same pixels by different paths: it accepts any
  path whose error stays within BABL_TOLERANCE (4.7e-6), picks by cost,
  and times its lookup tables (babl_ticks) to decide whether to use them
  (babl/babl-fish-path.c, master, read 2026-09-28). So two processes, or
  one under load, can read a u8 or u16 layer as floats a few 1e-6 apart;
  "R'G'B' float" and "R'G'B'A float" differed too. Mertens' weights are
  discontinuous where no frame has contrast, so such a difference can
  change a pixel of the result visibly (0.004 seen once). The GIMP tests
  therefore compare with the layers exactly as the plug-in read them
  (FUSION_DUMP_INPUTS).
- OpenCV's MergeMertens matches Mertens' method best with one pyramid
  level more than his code uses (measured, not read in OpenCV's source).
- The IPOL article on exposure fusion (Hessel 2018) gives the pyramid
  kernel as [.05 .25 .4 .25 .05]; Mertens' code uses [1 4 6 4 1]/16.
