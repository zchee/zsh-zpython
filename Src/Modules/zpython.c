#include "zpython.mdh"
#include "zpython.pro"
#include <Python.h>

#define PYTHON_SAVE_THREAD saved_python_thread = PyEval_SaveThread()
#define PYTHON_RESTORE_THREAD PyEval_RestoreThread(saved_python_thread); \
                              saved_python_thread = NULL

static PyThreadState *saved_python_thread = NULL;
static PyObject *globals;

/**/
static int
do_zpython(char *nam, char **args, Options ops, int func)
{
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
        case PM_ARRAY:
            PyErr_SetString(PyExc_NotImplementedError, "Hashes and arrays are currently not supported");
            return NULL;
        case PM_INTEGER:
            return PyLong_FromLong((long) v->pm->gsu.i->getfn(v->pm));
        case PM_EFLOAT:
        case PM_FFLOAT:
            return PyFloat_FromDouble(v->pm->gsu.f->getfn(v->pm));
        case PM_SCALAR:
            {
            char *s, *buf, *bufstart;
            PyObject *r;
            s = v->pm->gsu.s->getfn(v->pm);
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
        default:
            PyErr_SetString(PyExc_SystemError, "Parameter has unknown type; should not happen.");
            return NULL;
    }
}

/*
 * createparam, createspecialhash
 *
 * isident for finding ValueErrors
 *
 * getiparam, !!!getnparam, getsparam, getaparam, gethparam
 * gethkparam
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

static struct PyMethodDef ZshMethods[] = {
    {"eval", ZshEval, 1, "Evaluate command in current shell context",},
    {"last_exit_code", ZshExitCode, 0, "Get last exit code"},
    {"columns", ZshColumns, 0, "Get number of columns"},
    {"lines", ZshLines, 0, "Get number of lines"},
    {"subshell", ZshSubshell, 0, "Get subshell recursion depth"},
    {"getvalue", ZshGetValue, 1, "Get parameter value"},
    {NULL, NULL, 0, NULL},
};

/**/
int
boot_(Module m)
{
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
