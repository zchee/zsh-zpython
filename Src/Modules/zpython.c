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
    char *saved_locale;
    size_t locale_size = 0;
    int exit_code = 0;
    PyObject *result;

    saved_locale = setlocale(LC_NUMERIC, NULL);
    if(saved_locale == NULL || strcmp(saved_locale, "C") == 0)
        saved_locale = NULL;
    else {
        char *saved_locale_copy;
        locale_size = (strlen(saved_locale)+1) * sizeof(char);
        saved_locale_copy = (char *) zalloc(locale_size);
        memcpy((void *) saved_locale_copy, (void *) saved_locale, locale_size);
        (void)setlocale(LC_NUMERIC, "C");
    }
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
    if(saved_locale != NULL)
    {
        (void)setlocale(LC_NUMERIC, saved_locale);
        zfree(saved_locale, locale_size);
    }
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

/**/
int
boot_(Module m)
{
    Py_Initialize();
    PyEval_InitThreads();
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
