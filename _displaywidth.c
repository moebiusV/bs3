/*
 * _displaywidth — C accelerator for Unicode display-width calculations.
 *
 * Provides the same results as the Python _char_width / get_display_width /
 * truncate_to_width / skip_display_cols methods, but runs the per-character
 * loop in C for ~100× less overhead on ASCII-heavy content.
 *
 * For non-ASCII codepoints the module calls Python's unicodedata.category()
 * and unicodedata.east_asian_width() so the width logic is *identical* to
 * the pure-Python fallback — no approximation.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Cached references to unicodedata functions */
static PyObject *ucd_category  = NULL;
static PyObject *ucd_ea_width  = NULL;

static int
ensure_unicodedata(void)
{
    if (ucd_category)
        return 0;
    PyObject *mod = PyImport_ImportModule("unicodedata");
    if (!mod)
        return -1;
    ucd_category  = PyObject_GetAttrString(mod, "category");
    ucd_ea_width  = PyObject_GetAttrString(mod, "east_asian_width");
    Py_DECREF(mod);
    return (ucd_category && ucd_ea_width) ? 0 : -1;
}

/*
 * Per-character display width.  Exactly mirrors the Python logic:
 *
 *   ASCII 0x20-0x7E            → 1  (fast path, no Python calls)
 *   category Mn/Me/Cf          → 0  (except variation selectors → 1)
 *   east_asian_width W/F       → 2
 *   category So at U+2600+     → 2  (emoji terminals render wide)
 *   everything else            → 1
 */
static int
char_width(Py_UCS4 cp)
{
    /* Fast path: printable ASCII */
    if (cp >= 0x20 && cp <= 0x7E)
        return 1;
    /* C0/C1 control characters */
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
        return 0;

    /* Non-ASCII: ask unicodedata for exact category */
    PyObject *ch = PyUnicode_FromOrdinal(cp);
    if (!ch)
        return 1;

    /* --- category check --- */
    PyObject *cat_obj = PyObject_CallOneArg(ucd_category, ch);
    if (!cat_obj) { Py_DECREF(ch); return 1; }
    const char *cat = PyUnicode_AsUTF8(cat_obj);
    if (!cat) { Py_DECREF(ch); Py_DECREF(cat_obj); return 1; }

    char c0 = cat[0], c1 = cat[1];

    if ((c0 == 'M' && (c1 == 'n' || c1 == 'e')) ||
        (c0 == 'C' && c1 == 'f')) {
        Py_DECREF(ch);
        Py_DECREF(cat_obj);
        /* Variation selectors U+FE00-FE0F → 1 */
        return (cp >= 0xFE00 && cp <= 0xFE0F) ? 1 : 0;
    }

    /* --- east_asian_width check --- */
    PyObject *ea_obj = PyObject_CallOneArg(ucd_ea_width, ch);
    if (!ea_obj) { Py_DECREF(ch); Py_DECREF(cat_obj); return 1; }
    const char *ea = PyUnicode_AsUTF8(ea_obj);
    if (!ea) { Py_DECREF(ch); Py_DECREF(cat_obj); Py_DECREF(ea_obj); return 1; }

    int result;
    if (ea[0] == 'W' || ea[0] == 'F')
        result = 2;
    else if (c0 == 'S' && c1 == 'o' && cp >= 0x2600)
        result = 2;
    else
        result = 1;

    Py_DECREF(ch);
    Py_DECREF(cat_obj);
    Py_DECREF(ea_obj);
    return result;
}

/* ------------------------------------------------------------------ */
/* display_width(text) → int                                          */
/* ------------------------------------------------------------------ */
static PyObject *
py_display_width(PyObject *self, PyObject *args)
{
    PyObject *text;
    if (!PyArg_ParseTuple(args, "U", &text))
        return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(text);
    int kind = PyUnicode_KIND(text);
    void *data = PyUnicode_DATA(text);

    Py_ssize_t width = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        Py_UCS4 cp = PyUnicode_READ(kind, data, i);
        width += char_width(cp);
    }
    return PyLong_FromSsize_t(width);
}

/* ------------------------------------------------------------------ */
/* truncate_to_width(text, max_w) → str                               */
/* ------------------------------------------------------------------ */
static PyObject *
py_truncate_to_width(PyObject *self, PyObject *args)
{
    PyObject *text;
    Py_ssize_t max_w;
    if (!PyArg_ParseTuple(args, "Un", &text, &max_w))
        return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(text);
    int kind = PyUnicode_KIND(text);
    void *data = PyUnicode_DATA(text);

    Py_ssize_t w = 0;
    Py_ssize_t end = len;  /* default: keep everything */

    for (Py_ssize_t i = 0; i < len; i++) {
        Py_UCS4 cp = PyUnicode_READ(kind, data, i);
        int cw = char_width(cp);
        if (cw == 0)
            continue;   /* zero-width chars always included */
        if (w + cw > max_w) {
            end = i;
            /* Include trailing zero-width chars up to this point */
            break;
        }
        w += cw;
    }

    /* Walk backwards from end to find the actual cut point,
       making sure we include zero-width chars that precede `end`
       but were already added by the "continue" above.  The simplest
       correct approach: rebuild using the same logic as Python. */
    w = 0;
    Py_ssize_t cut = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        Py_UCS4 cp = PyUnicode_READ(kind, data, i);
        int cw = char_width(cp);
        if (cw == 0) {
            cut = i + 1;  /* include zero-width */
            continue;
        }
        if (w + cw > max_w)
            break;
        w += cw;
        cut = i + 1;
    }

    return PyUnicode_Substring(text, 0, cut);
}

/* ------------------------------------------------------------------ */
/* skip_display_cols(text, cols) → str                                */
/* ------------------------------------------------------------------ */
static PyObject *
py_skip_display_cols(PyObject *self, PyObject *args)
{
    PyObject *text;
    Py_ssize_t cols;
    if (!PyArg_ParseTuple(args, "Un", &text, &cols))
        return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(text);
    int kind = PyUnicode_KIND(text);
    void *data = PyUnicode_DATA(text);

    Py_ssize_t w = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        Py_UCS4 cp = PyUnicode_READ(kind, data, i);
        int cw = char_width(cp);
        if (cw == 0)
            continue;
        if (w + cw > cols)
            return PyUnicode_Substring(text, i, len);
        w += cw;
        if (w >= cols) {
            /* Skip trailing zero-width chars that modify this char */
            Py_ssize_t j = i + 1;
            while (j < len) {
                Py_UCS4 cp2 = PyUnicode_READ(kind, data, j);
                if (char_width(cp2) != 0)
                    break;
                j++;
            }
            return PyUnicode_Substring(text, j, len);
        }
    }
    return PyUnicode_FromString("");
}

/* ------------------------------------------------------------------ */
/* Module definition                                                  */
/* ------------------------------------------------------------------ */
static PyMethodDef methods[] = {
    {"display_width",     py_display_width,     METH_VARARGS,
     "display_width(text) -> int\n\n"
     "Return the display width of text, accounting for East Asian wide\n"
     "characters, combining marks, and emoji."},
    {"truncate_to_width", py_truncate_to_width, METH_VARARGS,
     "truncate_to_width(text, max_w) -> str\n\n"
     "Truncate text to fit within max_w display columns."},
    {"skip_display_cols", py_skip_display_cols,  METH_VARARGS,
     "skip_display_cols(text, cols) -> str\n\n"
     "Skip cols display columns from the start of text, return the rest."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_displaywidth",
    "C accelerator for Unicode display-width calculations.",
    -1,
    methods
};

PyMODINIT_FUNC
PyInit__displaywidth(void)
{
    if (ensure_unicodedata() < 0)
        return NULL;
    return PyModule_Create(&module);
}
