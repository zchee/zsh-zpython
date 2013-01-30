#include "zpython.mdh"
#include "zpython.pro"
#include <Python.h>

#define PYTHON_SAVE_THREAD saved_python_thread = PyEval_SaveThread()
#define PYTHON_RESTORE_THREAD PyEval_RestoreThread(saved_python_thread); \
                              saved_python_thread = NULL

static PyThreadState *saved_python_thread = NULL;
static PyObject *globals;
static zlong zpython_subshell;
static PyObject *hashdict = NULL;

static void
after_fork()
{
    zpython_subshell = zsh_subshell;
    hashdict = NULL;
    PyOS_AfterFork();
}

/**/
static int
do_zpython(char *nam, char **args, Options ops, int func)
{
    if (zsh_subshell > zpython_subshell)
        after_fork();

    int exit_code = 0;
    PyObject *result;

    PYTHON_RESTORE_THREAD;

    result = PyRun_String(*args, Py_file_input, globals, globals);
    if(result == NULL)
    {
        if(PyErr_Occurred())
            PyErr_PrintEx(0);
        exit_code = 1;
    }
    else
        Py_DECREF(result);
    PyErr_Clear();

    PYTHON_SAVE_THREAD;
    return exit_code;
}

static struct builtin bintab[] = {
    BUILTIN("zpython", 0, do_zpython,  1, 1, 0, NULL, NULL),
};

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL,   0,
    NULL,   0,
    NULL,   0,
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

static PyObject *
ZshEval(UNUSED(PyObject *self), PyObject *args)
{
    char *command;

    if(!PyArg_ParseTuple(args, "s", &command))
        return NULL;

    execstring(command, 1, 0, "zpython");

    Py_RETURN_NONE;
}

static PyObject *
get_string(char *s)
{
    char *buf, *bufstart;
    PyObject *r;
    /* No need in \0 byte at the end since we are using
     * PyString_FromStringAndSize */
    buf = PyMem_New(char, strlen(s));
    bufstart = buf;
    while (*s) {
        *buf++ = (*s == Meta) ? (*++s ^ 32) : (*s);
        ++s;
    }
    r = PyString_FromStringAndSize(bufstart, (Py_ssize_t)(buf - bufstart));
    PyMem_Free(bufstart);
    return r;
}

static void
scanhashdict(HashNode hn, UNUSED(int flags))
{
    struct value v;
    PyObject *key, *val;

    if(hashdict == NULL)
        return;

    v.pm = (Param) hn;

    key = get_string(v.pm->node.nam);

    v.isarr = (PM_TYPE(v.pm->node.flags) & (PM_ARRAY|PM_HASHED));
    v.flags = 0;
    v.start = 0;
    v.end = -1;
    val = get_string(getstrvalue(&v));

    if(PyDict_SetItem(hashdict, key, val) == -1)
        hashdict = NULL;

    Py_DECREF(key);
    Py_DECREF(val);
}

static PyObject *
ZshGetValue(UNUSED(PyObject *self), PyObject *args)
{
    char *name;
    struct value vbuf;
    Value v;

    if(!PyArg_ParseTuple(args, "s", &name))
        return NULL;

    if(!isident(name)) {
        PyErr_SetString(PyExc_ValueError, "Parameter name is not an identifier");
        return NULL;
    }

    if(!(v = getvalue(&vbuf, &name, 1))) {
        PyErr_SetString(PyExc_KeyError, "Failed to find parameter");
        return NULL;
    }

    switch(PM_TYPE(v->pm->node.flags)) {
        case PM_HASHED:
            if(hashdict) {
                PyErr_SetString(PyExc_RuntimeError, "hashdict already used. "
                        "Do not try to get two hashes simultaneously in separate threads");
                return NULL;
            }
            else {
                PyObject *hd;
                HashTable ht;

                hashdict = PyDict_New();
                hd = hashdict;

                scanhashtable(v->pm->gsu.h->getfn(v->pm), 0, 0, 0, scanhashdict, 0);
                if (hashdict == NULL) {
                    Py_DECREF(hd);
                    return NULL;
                }

                hashdict = NULL;
                return hd;
            }
        case PM_ARRAY:
            v->arr = v->pm->gsu.a->getfn(v->pm);
            if (v->isarr) {
                char **ss = v->arr;
                size_t i = 0;
                PyObject *str;
                PyObject *r = PyList_New(arrlen(ss));
                while (*ss) {
                    str = get_string(*ss++);
                    if(PyList_SetItem(r, i++, str) == -1) {
                        Py_DECREF(r);
                        return NULL;
                    }
                }
                return r;
            }
            else {
                char *s;
                PyObject *str, *r;

                if (v->start < 0)
                    v->start += arrlen(v->arr);
                s = (v->start >= arrlen(v->arr) || v->start < 0) ?
                    (char *) "" : v->arr[v->start];
                if(!(str = get_string(s)))
                    return NULL;
                r = PyList_New(1);
                if(PyList_SetItem(r, 0, str) == -1) {
                    Py_DECREF(r);
                    return NULL;
                }
                return r;
            }
        case PM_INTEGER:
            return PyLong_FromLong((long) v->pm->gsu.i->getfn(v->pm));
        case PM_EFLOAT:
        case PM_FFLOAT:
            return PyFloat_FromDouble(v->pm->gsu.f->getfn(v->pm));
        case PM_SCALAR:
            return get_string(v->pm->gsu.s->getfn(v->pm));
        default:
            PyErr_SetString(PyExc_SystemError, "Parameter has unknown type; should not happen.");
            return NULL;
    }
}

static char *
get_chars(PyObject *str)
{
    char *val, *buf, *bufstart;
    Py_ssize_t len = 0;
    Py_ssize_t i = 0;
    Py_ssize_t buflen = 1;

    if (PyString_AsStringAndSize(str, &val, &len) == -1)
        return NULL;

    while (i < len)
        buflen += 1 + (imeta(val[i++]) ? 1 : 0);

    buf = zalloc(buflen * sizeof(char));
    bufstart = buf;

    while (len) {
        if (imeta(*val)) {
            *buf++ = Meta;
            *buf++ = *val ^ 32;
        }
        else
            *buf++ = *val;
        val++;
        len--;
    }
    *buf = '\0';

    return bufstart;
}

static PyObject *
ZshSetValue(UNUSED(PyObject *self), PyObject *args)
{
    char *name;
    PyObject *value;

    if(!PyArg_ParseTuple(args, "sO", &name, &value))
        return NULL;

    if(!isident(name)) {
        PyErr_SetString(PyExc_ValueError, "Parameter name is not an identifier");
        return NULL;
    }

    if (PyString_Check(value)) {
        char *s = get_chars(value);

        if (!setsparam(name, s)) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to assign string to the parameter");
            zsfree(s);
            return NULL;
        }
    }
    else if (PyInt_Check(value)) {
        if (!setiparam(name, (zlong) PyInt_AsLong(value))) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to assign integer parameter");
            return NULL;
        }
    }
    else if (PyLong_Check(value)) {
        if (!setiparam(name, (zlong) PyLong_AsLong(value))) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to assign long parameter");
            return NULL;
        }
    }
    else if (PyDict_Check(value)) {
        char **val, **valstart;
        PyObject *pkey, *pval;
        Py_ssize_t len, pos = 0;

        len = 2 * PyDict_Size(value) * sizeof(char *) + 1;
        val = (char **) zalloc(len);
        valstart = val;

#define FAIL_SETTING_HASH \
        while (val-- > valstart) \
            zsfree(*val); \
        zfree(valstart, len); \
        return NULL

        while(PyDict_Next(value, &pos, &pkey, &pval)) {
            if(!PyString_Check(pkey)) {
                PyErr_SetString(PyExc_TypeError, "Only string keys are allowed");
                FAIL_SETTING_HASH;
            }
            if(!PyString_Check(pval)) {
                PyErr_SetString(PyExc_TypeError, "Only string values are allowed");
                FAIL_SETTING_HASH;
            }
            *val++ = get_chars(pkey);
            *val++ = get_chars(pval);
        }
        *val = NULL;
        if(!sethparam(name, valstart)) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to set hash");
            return NULL;
        }
    }
    else if (value == Py_None) {
        unsetparam (name);
        if (errflag) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to delete parameter");
            return NULL;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "Cannot assign value of the given type");
        return NULL;
    }

    Py_RETURN_NONE;
}

/*
 * createparam, createspecialhash
 */

#define ZSH_GETLONG_FUNCTION(funcname, var) \
static PyObject * \
Zsh##funcname(UNUSED(PyObject *self), UNUSED(PyObject *args)) \
{ \
    return PyInt_FromLong((long) var); \
}

ZSH_GETLONG_FUNCTION(ExitCode, lastval)
ZSH_GETLONG_FUNCTION(Columns,  zterm_columns)
ZSH_GETLONG_FUNCTION(Lines,    zterm_lines)
ZSH_GETLONG_FUNCTION(Subshell, zsh_subshell)

static PyObject *
ZshPipeStatus(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    ssize_t i = 0;
    PyObject *r = PyList_New(numpipestats);
    PyObject *num;

    while (i < numpipestats) {
        if(!(num = PyInt_FromLong(pipestats[i]))) {
            Py_DECREF(r);
            return NULL;
        }
        if(PyList_SetItem(r, i, num) == -1) {
            Py_DECREF(r);
            return NULL;
        }
        i++;
    }
    return r;
}

static struct PyMethodDef ZshMethods[] = {
    {"eval", ZshEval, 1, "Evaluate command in current shell context",},
    {"last_exit_code", ZshExitCode, 0, "Get last exit code"},
    {"pipestatus", ZshPipeStatus, 0, "Get last pipe status"},
    {"columns", ZshColumns, 0, "Get number of columns"},
    {"lines", ZshLines, 0, "Get number of lines"},
    {"subshell", ZshSubshell, 0, "Get subshell recursion depth"},
    {"getvalue", ZshGetValue, 1, "Get parameter value"},
    {"setvalue", ZshSetValue, 2, "Set parameter value. Use None to unset"},
    {NULL, NULL, 0, NULL},
};

/**/
int
boot_(Module m)
{
    zpython_subshell = zsh_subshell;
    Py_Initialize();
    PyEval_InitThreads();
    Py_InitModule4("zsh", ZshMethods, (char *)NULL, (PyObject *)NULL, PYTHON_API_VERSION);
    globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    PYTHON_SAVE_THREAD;
    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(Module m)
{
    if(Py_IsInitialized()) {
        PYTHON_RESTORE_THREAD;
        Py_Finalize();
    }
    return 0;
}
