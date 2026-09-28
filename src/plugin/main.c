/* gimp-fusion: exposure fusion and focus stacking for GIMP 3
 *
 *   Image > Merge Exposures...           plug-in-fusion-merge-exposures
 *   Image > Focus Stack...               plug-in-fusion-focus-stack
 *   Image > Align Layers by Content...   plug-in-fusion-align-layers
 *
 * All three take the selected layers (two or more) or the visible ones,
 * bottom of the stack first. The first two add the result as a new layer
 * above the topmost of them (and the focus stack, if asked, a depth map);
 * the third moves the layers themselves. The work is done by the core in
 * src/core (see fz.h).
 *
 * Copyright 2026 David
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <string.h>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "fz.h"
#include "layers.h"

#define PROC_EXPOSURES "plug-in-fusion-merge-exposures"
#define PROC_FOCUS     "plug-in-fusion-focus-stack"
#define PROC_ALIGN     "plug-in-fusion-align-layers"
#define PLUG_IN_BINARY "gimp-fusion"
#define MENU_PATH      "<Image>/Image/[Arrange]"

typedef struct _Fusion      Fusion;
typedef struct _FusionClass FusionClass;

struct _Fusion
{
  GimpPlugIn parent_instance;
};

struct _FusionClass
{
  GimpPlugInClass parent_class;
};

#define FUSION_TYPE (fusion_get_type ())
GType fusion_get_type (void) G_GNUC_CONST;

static GList          *fusion_query_procedures (GimpPlugIn *plug_in);
static GimpProcedure  *fusion_create_procedure (GimpPlugIn *plug_in, const gchar *name);
static GimpValueArray *fusion_run (GimpProcedure *procedure, GimpRunMode run_mode,
                                   GimpImage *image, GimpDrawable **drawables,
                                   GimpProcedureConfig *config, gpointer run_data);

G_DEFINE_TYPE (Fusion, fusion, GIMP_TYPE_PLUG_IN)

GIMP_MAIN (FUSION_TYPE)

/* no translations yet */
static gboolean
fusion_set_i18n (GimpPlugIn *plug_in, const gchar *procedure_name, gchar **gettext_domain,
                 gchar **catalog_dir)
{
  return FALSE;
}

static void
fusion_class_init (FusionClass *klass)
{
  GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS (klass);

  plug_in_class->set_i18n = fusion_set_i18n;
  plug_in_class->query_procedures = fusion_query_procedures;
  plug_in_class->create_procedure = fusion_create_procedure;
}

static void
fusion_init (Fusion *fusion)
{
}

static GList *
fusion_query_procedures (GimpPlugIn *plug_in)
{
  GList *list = NULL;

  list = g_list_append (list, g_strdup (PROC_EXPOSURES));
  list = g_list_append (list, g_strdup (PROC_FOCUS));
  list = g_list_append (list, g_strdup (PROC_ALIGN));
  return list;
}

/* ------------------------------------------------------------ arguments */

static void
add_common (GimpProcedure *procedure, const char *default_align)
{
  gimp_procedure_add_choice_argument (procedure, "layers", "La_yers",
    "Which layers to use, bottom of the stack first",
    gimp_choice_new_with_values (
      "auto", FUSION_LAYERS_AUTO, "The selected ones, or all visible ones",
        "The selected layers if two or more are selected, otherwise the visible layers",
      "selected", FUSION_LAYERS_SELECTED, "The selected layers", NULL,
      "visible", FUSION_LAYERS_VISIBLE, "The visible layers", "At the top level",
      "all", FUSION_LAYERS_ALL, "All layers", "At the top level",
      NULL),
    "auto", G_PARAM_READWRITE);
  gimp_procedure_add_choice_argument (procedure, "align", "_Align first",
    "Align the layers to the middle one before merging (the layers themselves "
    "stay as they are)",
    gimp_choice_new_with_values (
      "none", FZ_ALIGN_NONE, "No: they are aligned", NULL,
      "translation", FZ_ALIGN_TRANSLATION, "Shift", "Handheld shots, stage drift",
      "scale", FZ_ALIGN_SCALE, "Shift and scale",
        "Focus stacks: the magnification changes with the focus",
      "similarity", FZ_ALIGN_SIMILARITY, "Shift, rotation and scale", NULL,
      NULL),
    default_align, G_PARAM_READWRITE);
  gimp_procedure_add_boolean_argument (procedure, "crop", "Crop _to the common area",
    "Crop the image to the area that all layers cover (after alignment)",
    FALSE, G_PARAM_READWRITE);
  gimp_procedure_add_int_argument (procedure, "levels", "Pyramid le_vels",
    "Levels of the Laplacian pyramid; 0: as many as the paper's code uses, "
    "log2 of the shorter side. More levels blend over larger distances",
    0, FZ_MAX_LEVELS, 0, G_PARAM_READWRITE);
  gimp_procedure_add_boolean_argument (procedure, "clip", "Cl_ip to the normal range",
    "Clip the result to 0..1; the blend can overshoot a little at strong edges "
    "(without clipping, a floating point image keeps those values)",
    TRUE, G_PARAM_READWRITE);
}

static GimpProcedure *
fusion_create_procedure (GimpPlugIn *plug_in, const gchar *name)
{
  GimpProcedure *procedure;

  procedure = gimp_image_procedure_new (plug_in, name, GIMP_PDB_PROC_TYPE_PLUGIN,
                                        fusion_run, NULL, NULL);
  gimp_procedure_set_image_types (procedure, "RGB*, GRAY*");
  gimp_procedure_set_sensitivity_mask (procedure,
                                       GIMP_PROCEDURE_SENSITIVE_DRAWABLE |
                                       GIMP_PROCEDURE_SENSITIVE_DRAWABLES |
                                       GIMP_PROCEDURE_SENSITIVE_NO_DRAWABLES);
  gimp_procedure_set_attribution (procedure, "David", "David, GPL-3.0-or-later", "2026");

  if (! strcmp (name, PROC_EXPOSURES))
    {
      gimp_procedure_set_menu_label (procedure, "Merge E_xposures...");
      gimp_procedure_set_documentation (procedure,
        "Merge differently exposed layers into one well exposed layer (exposure fusion)",
        "Exposure fusion after Mertens, Kautz and Van Reeth (2007): every pixel of every "
        "layer is weighed by its contrast, saturation and well-exposedness, and the layers "
        "are blended with those weights in a Laplacian pyramid, so that no seams show. "
        "No HDR image is made and no camera curve is needed. The result is a new layer "
        "above the topmost of the layers. Any number of layers, 8 or 16 bits, floating "
        "point, RGB or gray.",
        name);
      add_common (procedure, "none");
      gimp_procedure_add_double_argument (procedure, "contrast", "Co_ntrast",
        "Exponent of the contrast weight (the Laplacian's magnitude); 0 ignores it",
        0.0, 10.0, 1.0, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "saturation", "Sat_uration",
        "Exponent of the saturation weight (the spread of R, G and B); 0 ignores it",
        0.0, 10.0, 1.0, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "exposure", "_Well-exposedness",
        "Exponent of the well-exposedness weight (closeness to the middle); 0 ignores it",
        0.0, 10.0, 1.0, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "exposure-width", "Well-exposedness wi_dth",
        "Width (sigma) of the well-exposedness curve around 0.5; the paper uses 0.2",
        0.05, 1.0, 0.2, G_PARAM_READWRITE);
      gimp_procedure_add_choice_argument (procedure, "space", "_Blend in",
        "The values the weights and the blend work on",
        gimp_choice_new_with_values (
          "perceptual", FUSION_SPACE_PERCEPTUAL, "Perceptual values (as in the paper)",
            "Gamma encoded, like the camera JPEGs the method was made for",
          "linear", FUSION_SPACE_LINEAR, "Linear light", NULL,
          NULL),
        "perceptual", G_PARAM_READWRITE);
      gimp_procedure_add_layer_return_value (procedure, "layer", "Layer",
        "The merged layer", FALSE, G_PARAM_READWRITE);
    }
  else if (! strcmp (name, PROC_FOCUS))
    {
      gimp_procedure_set_menu_label (procedure, "_Focus Stack...");
      gimp_procedure_set_documentation (procedure,
        "Merge layers focused at different depths into one sharp layer (focus stacking)",
        "Measures the sharpness of every layer at every pixel and keeps the sharpest, "
        "blended in a Laplacian pyramid so that no seams show. Optionally adds a depth "
        "map: which layer was sharpest where, from black (the bottom layer) to white "
        "(the top one). Works on 8 and 16 bits and floating point, RGB or gray, for "
        "macro photography and microscopy.",
        name);
      add_common (procedure, "none");
      gimp_procedure_add_choice_argument (procedure, "method", "M_ethod",
        "How the sharpest layer is used",
        gimp_choice_new_with_values (
          "pyramid", FZ_FOCUS_PYRAMID, "Sharpest layer per pixel, blended",
            "Masks of the sharpest layer, blended in a pyramid (as enfuse's hard mask)",
          "coefficients", FZ_FOCUS_COEFF, "Sharpest detail per scale",
            "At every level of the pyramid the strongest detail (Burt and Kolczynski)",
          "soft", FZ_FOCUS_SOFT, "Weighted by sharpness",
            "Weights sharpness^power: smoother, less noise, a little softer",
          "pixels", FZ_FOCUS_PIXELS, "Sharpest layer per pixel, not blended",
            "The pixels of the layers as they are: for measurements",
          NULL),
        "pyramid", G_PARAM_READWRITE);
      gimp_procedure_add_choice_argument (procedure, "measure", "Sharpness _measure",
        "How sharpness is measured",
        gimp_choice_new_with_values (
          "laplacian", FZ_MEASURE_LAPLACIAN, "Energy of the Laplacian", NULL,
          "variance", FZ_MEASURE_VARIANCE, "Local variance", NULL,
          NULL),
        "laplacian", G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "noise", "_Noise smoothing",
        "Blur (sigma, pixels) before measuring, so that noise does not count as detail",
        0.0, 10.0, 1.0, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "window", "Measuring _window",
        "Size (sigma, pixels) of the area the sharpness is measured over",
        0.5, 20.0, 2.0, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "power", "_Power",
        "For Weighted by sharpness: the exponent of the weights",
        0.5, 20.0, 4.0, G_PARAM_READWRITE);
      gimp_procedure_add_choice_argument (procedure, "space", "_Blend in",
        "The values the blend works on",
        gimp_choice_new_with_values (
          "image", FUSION_SPACE_IMAGE, "As the image stores them", NULL,
          "perceptual", FUSION_SPACE_PERCEPTUAL, "Perceptual values", NULL,
          "linear", FUSION_SPACE_LINEAR, "Linear light", NULL,
          NULL),
        "image", G_PARAM_READWRITE);
      gimp_procedure_add_boolean_argument (procedure, "depth-map", "Add a _depth map",
        "Also add a layer showing which layer was sharpest where: black is the bottom "
        "layer, white the top one, in between with a fraction from the neighbours",
        FALSE, G_PARAM_READWRITE);
      gimp_procedure_add_double_argument (procedure, "depth-smooth", "Depth map smoothin_g",
        "Smooth the depth map (sigma, pixels), weighing by sharpness; 0: none",
        0.0, 50.0, 4.0, G_PARAM_READWRITE);
      gimp_procedure_add_layer_return_value (procedure, "layer", "Layer",
        "The focus stacked layer", FALSE, G_PARAM_READWRITE);
      gimp_procedure_add_layer_return_value (procedure, "depth-layer", "Depth layer",
        "The depth map, if asked for", TRUE, G_PARAM_READWRITE);
    }
  else if (! strcmp (name, PROC_ALIGN))
    {
      gimp_procedure_set_menu_label (procedure, "Align Layers by Co_ntent...");
      gimp_procedure_set_documentation (procedure,
        "Move, rotate and scale layers so that their contents line up",
        "Aligns the layers to the middle one of them by their contents: phase "
        "correlation, then a least squares fit to a fraction of a pixel, each layer "
        "against its neighbour. Works for brackets (other exposures) and focus stacks. "
        "The layers are transformed with GIMP's interpolation and keep their size.",
        name);
      gimp_procedure_add_choice_argument (procedure, "layers", "La_yers",
        "Which layers to align, bottom of the stack first",
        gimp_choice_new_with_values (
          "auto", FUSION_LAYERS_AUTO, "The selected ones, or all visible ones", NULL,
          "selected", FUSION_LAYERS_SELECTED, "The selected layers", NULL,
          "visible", FUSION_LAYERS_VISIBLE, "The visible layers", NULL,
          "all", FUSION_LAYERS_ALL, "All layers", NULL,
          NULL),
        "auto", G_PARAM_READWRITE);
      gimp_procedure_add_choice_argument (procedure, "model", "_Transform",
        "What may differ between the layers",
        gimp_choice_new_with_values (
          "translation", FZ_ALIGN_TRANSLATION, "Shift", NULL,
          "scale", FZ_ALIGN_SCALE, "Shift and scale", NULL,
          "similarity", FZ_ALIGN_SIMILARITY, "Shift, rotation and scale", NULL,
          NULL),
        "translation", G_PARAM_READWRITE);
      gimp_procedure_add_choice_argument (procedure, "content", "_Content",
        "What the layers show",
        gimp_choice_new_with_values (
          "exposures", 1, "Other exposures", "Compared by local contrast",
          "focus", 0, "Other focus, same exposure", "Compared by brightness",
          NULL),
        "exposures", G_PARAM_READWRITE);
    }
  /* (after the menu label, which GIMP wants first) */
  gimp_procedure_add_menu_path (procedure, MENU_PATH);
  return procedure;
}

/* ---------------------------------------------------------------- dialogs */

static gboolean
show_dialog (GimpProcedure *procedure, GimpProcedureConfig *config, const char *name)
{
  GtkWidget *dialog;
  gboolean   ok;

  gimp_ui_init (PLUG_IN_BINARY);
  if (! strcmp (name, PROC_EXPOSURES))
    {
      dialog = gimp_procedure_dialog_new (procedure, config, "Merge Exposures");
      gimp_procedure_dialog_get_label (GIMP_PROCEDURE_DIALOG (dialog), "weights-title",
                                       "Weights (exponents)", FALSE, FALSE);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "contrast", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "saturation", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "exposure", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "exposure-width", 1.0);
      gimp_procedure_dialog_fill_box (GIMP_PROCEDURE_DIALOG (dialog), "weights-box",
                                      "contrast", "saturation", "exposure", "exposure-width",
                                      NULL);
      gimp_procedure_dialog_fill_frame (GIMP_PROCEDURE_DIALOG (dialog), "weights-frame",
                                        "weights-title", FALSE, "weights-box");
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "space",
                                        GIMP_TYPE_INT_RADIO_FRAME);
      gimp_procedure_dialog_get_label (GIMP_PROCEDURE_DIALOG (dialog), "note",
        "The result is a new layer above the topmost of the layers.", FALSE, FALSE);
      gimp_procedure_dialog_fill (GIMP_PROCEDURE_DIALOG (dialog), "layers", "weights-frame",
                                  "space", "align", "crop", "levels", "clip", "note", NULL);
    }
  else if (! strcmp (name, PROC_FOCUS))
    {
      GimpValueArray *soft = gimp_value_array_new (1);
      GValue          v = G_VALUE_INIT;

      dialog = gimp_procedure_dialog_new (procedure, config, "Focus Stack");
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "method",
                                        GIMP_TYPE_INT_RADIO_FRAME);
      /* combo boxes for the rest: the dialog stays short enough for small
       * screens */
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "measure",
                                        GTK_TYPE_COMBO_BOX);
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "space",
                                        GTK_TYPE_COMBO_BOX);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "noise", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "window", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "power", 1.0);
      gimp_procedure_dialog_get_spin_scale (GIMP_PROCEDURE_DIALOG (dialog), "depth-smooth", 1.0);
      g_value_init (&v, G_TYPE_STRING);
      g_value_set_string (&v, "soft");
      gimp_value_array_append (soft, &v);
      g_value_unset (&v);
      /* (the dialog takes the array: it unrefs it when it goes) */
      gimp_procedure_dialog_set_sensitive_if_in (GIMP_PROCEDURE_DIALOG (dialog), "power",
                                                 NULL, "method", soft, TRUE);
      gimp_procedure_dialog_set_sensitive (GIMP_PROCEDURE_DIALOG (dialog), "depth-smooth",
                                           TRUE, NULL, "depth-map", FALSE);
      gimp_procedure_dialog_fill_box (GIMP_PROCEDURE_DIALOG (dialog), "sharp-box",
                                      "measure", "noise", "window", "power", NULL);
      gimp_procedure_dialog_get_label (GIMP_PROCEDURE_DIALOG (dialog), "sharp-title",
                                       "Sharpness", FALSE, FALSE);
      gimp_procedure_dialog_fill_frame (GIMP_PROCEDURE_DIALOG (dialog), "sharp-frame",
                                        "sharp-title", FALSE, "sharp-box");
      gimp_procedure_dialog_fill_box (GIMP_PROCEDURE_DIALOG (dialog), "depth-box",
                                      "depth-smooth", NULL);
      gimp_procedure_dialog_fill_frame (GIMP_PROCEDURE_DIALOG (dialog), "depth-frame",
                                        "depth-map", FALSE, "depth-box");
      gimp_procedure_dialog_get_label (GIMP_PROCEDURE_DIALOG (dialog), "note",
        "The result is a new layer above the topmost of the layers.", FALSE, FALSE);
      gimp_procedure_dialog_fill (GIMP_PROCEDURE_DIALOG (dialog), "layers", "method",
                                  "sharp-frame", "space", "align", "crop", "depth-frame",
                                  "levels", "clip", "note", NULL);
    }
  else
    {
      dialog = gimp_procedure_dialog_new (procedure, config, "Align Layers by Content");
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "model",
                                        GIMP_TYPE_INT_RADIO_FRAME);
      gimp_procedure_dialog_get_widget (GIMP_PROCEDURE_DIALOG (dialog), "content",
                                        GIMP_TYPE_INT_RADIO_FRAME);
      gimp_procedure_dialog_get_label (GIMP_PROCEDURE_DIALOG (dialog), "note",
        "The layers are aligned to the middle one of them.", FALSE, FALSE);
      gimp_procedure_dialog_fill (GIMP_PROCEDURE_DIALOG (dialog), "layers", "model",
                                  "content", "note", NULL);
    }
  ok = gimp_procedure_dialog_run (GIMP_PROCEDURE_DIALOG (dialog));
  gtk_widget_destroy (dialog);
  return ok;
}

/* ------------------------------------------------------------------ run */

typedef struct
{
  double from, to;
} ProgressRange;

static void
progress (void *data, double fraction)
{
  ProgressRange *r = data;

  gimp_progress_update (r->from + (r->to - r->from) * fraction);
}

static gboolean
get_boolean (GimpProcedureConfig *config, const char *name)
{
  gboolean v = FALSE;

  g_object_get (config, name, &v, NULL);
  return v;
}

static GimpValueArray *
fail (GimpProcedure *procedure, GError *error)
{
  return gimp_procedure_new_return_values (procedure, GIMP_PDB_EXECUTION_ERROR, error);
}

static GimpValueArray *
fail_msg (GimpProcedure *procedure, const char *what, char *msg)
{
  GError *error = g_error_new (GIMP_PLUG_IN_ERROR, 0, "%s: %s", what,
                               msg ? msg : "it failed");

  free (msg);
  return fail (procedure, error);
}

/* statistics for tests and benchmarks, when FUSION_STATS is set: on
 * stderr, where GIMP's log catches them. The peak memory is VmHWM of
 * /proc/self/status: getrusage's ru_maxrss survives execve, so in a
 * plug-in (forked from GIMP) it would be GIMP's own peak. */
static void
stats (const char *what, int n, int w, int h, double seconds, int loads)
{
  gchar *status = NULL, *hwm;
  double mb = -1.0;

  if (! g_getenv ("FUSION_STATS"))
    return;
  if (g_file_get_contents ("/proc/self/status", &status, NULL, NULL) &&
      (hwm = strstr (status, "VmHWM:")))
    mb = g_ascii_strtod (hwm + 6, NULL) / 1024.0;
  g_free (status);
  g_printerr ("FUSION_STATS %s layers %d size %dx%d seconds %.3f peak_mb %.1f loads %d\n",
              what, n, w, h, seconds, mb, loads);
}

static GimpValueArray *
run_align_layers (GimpProcedure *procedure, GimpImage *image, GimpProcedureConfig *config)
{
  GError        *error = NULL;
  GimpLayer    **layers;
  LayerSource    ls;
  FzAlignParams  ap = { FZ_ALIGN_TRANSLATION, -1, 0, 1 };
  FzTransform   *t;
  FzProgress     prog = { 0 };
  ProgressRange  range = { 0.0, 0.9 };
  char          *err = NULL;
  int            n, k;
  double         cx, cy;

  layers = fusion_collect_layers (image,
                                  gimp_procedure_config_get_choice_id (config, "layers"),
                                  &n, &error);
  if (! layers)
    return fail (procedure, error);
  ap.model = gimp_procedure_config_get_choice_id (config, "model");
  ap.normalize = gimp_procedure_config_get_choice_id (config, "content");
  /* compared in perceptual values: other exposures look alike there */
  fusion_layer_source_init (&ls, image, layers, n, FUSION_SPACE_PERCEPTUAL);
  t = g_new (FzTransform, n);
  prog.progress = progress;
  prog.data = &range;
  gimp_progress_init ("Aligning layers");
  if (! fz_align (&ls.src, &ap, t, NULL, &prog, &err))
    {
      g_free (t);
      g_free (layers);
      return fail_msg (procedure, "Align Layers by Content", err);
    }
  cx = 0.5 * (ls.src.w - 1);
  cy = 0.5 * (ls.src.h - 1);
  gimp_image_undo_group_start (image);
  gimp_context_push ();
  gimp_context_set_transform_resize (GIMP_TRANSFORM_RESIZE_CLIP);
  for (k = 0; k < n; k++)
    {
      FzTransform inv;
      double      tx, ty;

      if (t[k].a == 1.0 && t[k].b == 0.0 && t[k].tx == 0.0 && t[k].ty == 0.0)
        continue;
      /* the layer's content at q (its pixel centers at integers) goes to
       * p = inverse (q); in GIMP's coordinates, where pixel centers are at
       * .5: P = A Q + (I - A) (c + 0.5) + t */
      fz_transform_invert (&t[k], &inv);
      tx = (1.0 - inv.a) * (cx + 0.5) + inv.b * (cy + 0.5) + inv.tx;
      ty = -inv.b * (cx + 0.5) + (1.0 - inv.a) * (cy + 0.5) + inv.ty;
      gimp_item_transform_matrix (GIMP_ITEM (layers[k]),
                                  inv.a, -inv.b, tx,
                                  inv.b, inv.a, ty,
                                  0.0, 0.0, 1.0);
    }
  gimp_context_pop ();
  gimp_image_undo_group_end (image);
  gimp_progress_update (1.0);
  gimp_displays_flush ();
  g_free (t);
  g_free (layers);
  return gimp_procedure_new_return_values (procedure, GIMP_PDB_SUCCESS, NULL);
}

static GimpValueArray *
fusion_run (GimpProcedure *procedure, GimpRunMode run_mode, GimpImage *image,
            GimpDrawable **drawables, GimpProcedureConfig *config, gpointer run_data)
{
  const char     *name = gimp_procedure_get_name (procedure);
  gboolean        focus = ! strcmp (name, PROC_FOCUS);
  GError         *error = NULL;
  GimpLayer     **layers;
  GimpLayer      *result = NULL, *depth_layer = NULL;
  LayerSource     ls;
  FzImage        *out = NULL;
  float          *alpha = NULL, *depth = NULL;
  FzTransform    *t = NULL;
  FzWarpSource   *ws = NULL;
  const FzSource *src;
  FzProgress      prog = { 0 };
  ProgressRange   range = { 0.0, 1.0 };
  char           *err = NULL;
  int             n, align, ok;
  gint64          t0 = g_get_monotonic_time ();
  GimpValueArray *ret;

  fz_set_threads (gimp_get_num_processors ());
  if (run_mode == GIMP_RUN_INTERACTIVE && ! show_dialog (procedure, config, name))
    return gimp_procedure_new_return_values (procedure, GIMP_PDB_CANCEL, NULL);
  if (! strcmp (name, PROC_ALIGN))
    return run_align_layers (procedure, image, config);

  layers = fusion_collect_layers (image,
                                  gimp_procedure_config_get_choice_id (config, "layers"),
                                  &n, &error);
  if (! layers)
    return fail (procedure, error);
  fusion_layer_source_init (&ls, image, layers, n,
                            gimp_procedure_config_get_choice_id (config, "space"));
  src = &ls.src;
  prog.progress = progress;
  prog.data = &range;
  gimp_progress_init (focus ? "Focus stacking" : "Merging exposures");

  align = gimp_procedure_config_get_choice_id (config, "align");
  if (align != FZ_ALIGN_NONE)
    {
      FzAlignParams ap = { align, -1, 0, ! focus };

      t = g_new (FzTransform, n);
      range.to = 0.3;
      if (! fz_align (src, &ap, t, NULL, &prog, &err))
        {
          ret = fail_msg (procedure, focus ? "Focus Stack" : "Merge Exposures", err);
          goto done;
        }
      ws = fz_warp_source_new (src, t);
      if (! ws)
        {
          ret = fail_msg (procedure, "Merge Exposures", fz_strdup_printf ("not enough memory"));
          goto done;
        }
      src = fz_warp_source_get (ws);
      range.from = 0.3;
      range.to = 1.0;
    }

  out = fz_image_new (src->w, src->h, src->c);
  alpha = g_try_malloc (sizeof (float) * src->w * src->h);
  if (focus && get_boolean (config, "depth-map"))
    depth = g_try_malloc (sizeof (float) * src->w * src->h);
  if (! out || ! alpha || (focus && get_boolean (config, "depth-map") &&
                           ! depth))
    {
      ret = fail_msg (procedure, name, fz_strdup_printf ("not enough memory for %d x %d",
                                                         src->w, src->h));
      goto done;
    }

  if (focus)
    {
      FzFocusParams fp;
      gdouble       v;

      fz_focus_params_default (&fp);
      fp.method = gimp_procedure_config_get_choice_id (config, "method");
      fp.measure = gimp_procedure_config_get_choice_id (config, "measure");
      g_object_get (config, "noise", &v, NULL);
      fp.noise = v;
      g_object_get (config, "window", &v, NULL);
      fp.window = v;
      g_object_get (config, "power", &v, NULL);
      fp.power = v;
      g_object_get (config, "depth-smooth", &v, NULL);
      fp.depth_smooth = v;
      g_object_get (config, "levels", &fp.levels, "clip", &fp.clip, NULL);
      ok = fz_focus_stack (src, &fp, out, alpha, depth, &prog, &err);
    }
  else
    {
      FzExposureParams ep;

      fz_exposure_params_default (&ep);
      g_object_get (config, "contrast", &ep.contrast, "saturation", &ep.saturation,
                    "exposure", &ep.exposure, "exposure-width", &ep.sigma,
                    "levels", &ep.levels, "clip", &ep.clip, NULL);
      ok = fz_fuse_exposures (src, &ep, out, alpha, &prog, &err);
    }
  if (! ok)
    {
      ret = fail_msg (procedure, focus ? "Focus Stack" : "Merge Exposures", err);
      goto done;
    }

  gimp_image_undo_group_start (image);
  {
    /* an alpha channel only where it is needed */
    gboolean need_alpha = FALSE;
    size_t   i;

    for (i = 0; i < (size_t) src->w * src->h && ! need_alpha; i++)
      need_alpha = alpha[i] < 1.0f;
    result = fusion_add_layer (&ls, focus ? "Focus stack" : "Merged exposures", out,
                               need_alpha || ls.any_alpha ? alpha : NULL, layers[n - 1]);
  }
  if (result && depth)
    {
      size_t i;

      for (i = 0; i < (size_t) src->w * src->h; i++)
        if (depth[i] >= 0.0f)
          depth[i] = n > 1 ? depth[i] / (n - 1) : 0.0f;
      depth_layer = fusion_add_value_layer (&ls, "Focus stack depth", depth, result);
      /* the stacked layer on top, the depth map under it, hidden */
      if (depth_layer)
        {
          gimp_image_lower_item (image, GIMP_ITEM (depth_layer));
          gimp_item_set_visible (GIMP_ITEM (depth_layer), FALSE);
        }
    }
  if (result && get_boolean (config, "crop"))
    {
      int x, y, w, h, ax = 0, ay = 0, aw = src->w, ah = src->h;

      fusion_layers_cover (&ls, &x, &y, &w, &h);
      if (t)
        fz_common_area (src->w, src->h, n, t, &ax, &ay, &aw, &ah);
      {
        int x0 = MAX (x, ax), y0 = MAX (y, ay);
        int x1 = MIN (x + w, ax + aw), y1 = MIN (y + h, ay + ah);

        if (x1 > x0 && y1 > y0 && (x1 - x0 < src->w || y1 - y0 < src->h))
          gimp_image_crop (image, x1 - x0, y1 - y0, x0, y0);
      }
    }
  if (result)
    {
      GimpLayer *sel[2] = { result, NULL };

      gimp_image_set_selected_layers (image, (const GimpLayer **) sel);
    }
  gimp_image_undo_group_end (image);
  gimp_progress_update (1.0);
  gimp_displays_flush ();
  stats (focus ? "focus" : "exposures", n, src->w, src->h,
         (g_get_monotonic_time () - t0) / 1e6, ls.loads);

  if (! result)
    {
      ret = fail (procedure, g_error_new (GIMP_PLUG_IN_ERROR, 0, "could not add the layer"));
      goto done;
    }
  ret = gimp_procedure_new_return_values (procedure, GIMP_PDB_SUCCESS, NULL);
  GIMP_VALUES_SET_LAYER (ret, 1, result);
  if (focus)
    GIMP_VALUES_SET_LAYER (ret, 2, depth_layer);

done:
  fz_warp_source_free (ws);
  g_free (t);
  fz_image_free (out);
  g_free (alpha);
  g_free (depth);
  g_free (layers);
  return ret;
}
