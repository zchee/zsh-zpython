#include "zpython.mdh"
#include "zpython.pro"
#include <Python.h>

#define PYTHON_SAVE_THREAD saved_python_thread = PyEval_SaveThread()
#define PYTHON_RESTORE_THREAD PyEval_RestoreThread(saved_python_thread); \
			      saved_python_thread = NULL

struct specialparam {
    char *name;
    Param pm;
    struct specialparam *next;
    struct specialparam *prev;
};

struct special_data {
    struct specialparam *sp;
    PyObject *obj;
};

struct obj_hash_node {
    HashNode next;
    char *nam;
    int flags;
    PyObject *obj;
    struct specialparam *sp;
};

static PyThreadState *saved_python_thread = NULL;
static PyObject *globals;
static zlong zpython_subshell;
static PyObject *hashdict = NULL;
static struct specialparam *first_assigned_param = NULL;
static struct specialparam *last_assigned_param = NULL;


static void
after_fork()
{
    zpython_subshell = zsh_subshell;
    hashdict = NULL;
    PyOS_AfterFork();
}

#define PYTHON_INIT(failval) \
    PYTHON_RESTORE_THREAD; \
 \
    if (zsh_subshell > zpython_subshell) { \
	after_fork(); \
    }

#define PYTHON_FINISH \
    fflush(stderr); \
    fflush(stdout); \
    PYTHON_SAVE_THREAD

/**/
static int
do_zpython(char *nam, char **args, Options ops, int func)
{
    PyObject *result;
    int exit_code = 0;

    PYTHON_INIT(2);

    result = PyRun_String(*args, Py_file_input, globals, globals);
    if (result == NULL)
    {
	if (PyErr_Occurred()) {
	    PyErr_PrintEx(0);
	    exit_code = 1;
	}
    }
    else
	Py_DECREF(result);
    PyErr_Clear();

    PYTHON_FINISH;
    return exit_code;
}

static PyObject *
ZshEval(UNUSED(PyObject *self), PyObject *args)
{
    char *command;

    if (!PyArg_ParseTuple(args, "s", &command))
	return NULL;

    execstring(command, 1, 0, "zpython");

    Py_RETURN_NONE;
}

static PyObject *
get_string(const char *s)
{
    char *buf, *bufstart;
    PyObject *r;
    /* No need in \0 byte at the end since we are using
     * PyString_FromStringAndSize */
    if (!(buf = PyMem_New(char, strlen(s))))
	return NULL;
    bufstart = buf;
    while (*s) {
	*buf++ = (*s == Meta) ? (*++s ^ 32) : (*s);
	++s;
    }
    r = PyString_FromStringAndSize(bufstart, (Py_ssize_t) (buf - bufstart));
    PyMem_Free(bufstart);
    return r;
}

static void
scanhashdict(HashNode hn, UNUSED(int flags))
{
    struct value v;
    PyObject *key, *val;

    if (hashdict == NULL)
	return;

    v.pm = (Param) hn;

    if (!(key = get_string(v.pm->node.nam))) {
	hashdict = NULL;
	return;
    }

    v.isarr = (PM_TYPE(v.pm->node.flags) & (PM_ARRAY|PM_HASHED));
    v.flags = 0;
    v.start = 0;
    v.end = -1;
    if (!(val = get_string(getstrvalue(&v)))) {
	hashdict = NULL;
	Py_DECREF(key);
	return;
    }

    if (PyDict_SetItem(hashdict, key, val) == -1)
	hashdict = NULL;

    Py_DECREF(key);
    Py_DECREF(val);
}

static PyObject *
get_array(char **ss)
{
    PyObject *r = PyList_New(arrlen(ss));
    size_t i = 0;
    while (*ss) {
	PyObject *str;
	if (!(str = get_string(*ss++))) {
	    Py_DECREF(r);
	    return NULL;
	}
	if (PyList_SetItem(r, i++, str) == -1) {
	    Py_DECREF(r);
	    return NULL;
	}
    }
    return r;
}

static PyObject *
get_hash(HashTable ht)
{
    PyObject *hd;

    if (hashdict) {
	PyErr_SetString(PyExc_RuntimeError, "hashdict already used. "
		"Do not try to get two hashes simultaneously in "
		"separate threads, zsh is not thread-safe");
	return NULL;
    }

    hashdict = PyDict_New();
    hd = hashdict;

    scanhashtable(ht, 0, 0, 0, scanhashdict, 0);
    if (hashdict == NULL) {
	Py_DECREF(hd);
	return NULL;
    }

    hashdict = NULL;
    return hd;
}

static PyObject *
ZshGetValue(UNUSED(PyObject *self), PyObject *args)
{
    char *name;
    struct value vbuf;
    Value v;

    if (!PyArg_ParseTuple(args, "s", &name))
	return NULL;

    if (!isident(name)) {
	PyErr_SetString(PyExc_KeyError, "Parameter name is not an identifier");
	return NULL;
    }

    if (!(v = getvalue(&vbuf, &name, 1))) {
	PyErr_SetString(PyExc_IndexError, "Failed to find parameter");
	return NULL;
    }

    switch (PM_TYPE(v->pm->node.flags)) {
    case PM_HASHED:
	return get_hash(v->pm->gsu.h->getfn(v->pm));
    case PM_ARRAY:
	v->arr = v->pm->gsu.a->getfn(v->pm);
	if (v->isarr) {
	    return get_array(v->arr);
	}
	else {
	    char *s;
	    PyObject *str, *r;

	    if (v->start < 0)
		v->start += arrlen(v->arr);
	    s = (v->start >= arrlen(v->arr) || v->start < 0) ?
		(char *) "" : v->arr[v->start];
	    if (!(str = get_string(s)))
		return NULL;
	    r = PyList_New(1);
	    if (PyList_SetItem(r, 0, str) == -1) {
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
	PyErr_SetString(PyExc_SystemError,
		"Parameter has unknown type; should not happen.");
	return NULL;
    }
}

typedef void *(*Allocator) (size_t);

static char *
get_chars(PyObject *string, Allocator alloc)
{
    char *str, *buf, *bufstart;
    Py_ssize_t len = 0;
    Py_ssize_t i = 0;
    Py_ssize_t buflen = 1;

    if (PyString_AsStringAndSize(string, &str, &len) == -1)
	return NULL;

    while (i < len)
	buflen += 1 + (imeta(str[i++]) ? 1 : 0);

    buf = alloc(buflen * sizeof(char));
    bufstart = buf;

    while (len) {
	if (imeta(*str)) {
	    *buf++ = Meta;
	    *buf++ = *str ^ 32;
	}
	else
	    *buf++ = *str;
	str++;
	len--;
    }
    *buf = '\0';

    return bufstart;
}

#define FAIL_SETTING_ARRAY \
	while (val-- > valstart) \
	    zsfree(*val); \
	zfree(valstart, arrlen); \
	return NULL

static char **
get_chars_array(PyObject *seq, Allocator alloc)
{
    char **val, **valstart;
    Py_ssize_t len = PySequence_Size(seq);
    Py_ssize_t arrlen;
    Py_ssize_t i = 0;

    if (len == -1) {
	PyErr_SetString(PyExc_ValueError, "Failed to get sequence size");
	return NULL;
    }

    arrlen = (len + 1) * sizeof(char *);
    val = (char **) alloc(arrlen);
    valstart = val;

    while (i < len) {
	PyObject *item = PySequence_GetItem(seq, i);

	if (!PyString_Check(item)) {
	    PyErr_SetString(PyExc_TypeError, "Sequence item is not a string");
	    FAIL_SETTING_ARRAY;
	}

	*val++ = get_chars(item, alloc);
	i++;
    }
    *val = NULL;

    return valstart;
}

static PyObject *
ZshSetValue(UNUSED(PyObject *self), PyObject *args)
{
    char *name;
    PyObject *value;

    if (!PyArg_ParseTuple(args, "sO", &name, &value))
	return NULL;

    if (!isident(name)) {
	PyErr_SetString(PyExc_KeyError, "Parameter name is not an identifier");
	return NULL;
    }

    if (PyString_Check(value)) {
	char *s = get_chars(value, zalloc);

	if (!setsparam(name, s)) {
	    PyErr_SetString(PyExc_RuntimeError,
		    "Failed to assign string to the parameter");
	    zsfree(s);
	    return NULL;
	}
    }
    else if (PyInt_Check(value)) {
	if (!setiparam(name, (zlong) PyInt_AsLong(value))) {
	    PyErr_SetString(PyExc_RuntimeError,
		    "Failed to assign integer parameter");
	    return NULL;
	}
    }
    else if (PyLong_Check(value)) {
	if (!setiparam(name, (zlong) PyLong_AsLong(value))) {
	    PyErr_SetString(PyExc_RuntimeError,
		    "Failed to assign long parameter");
	    return NULL;
	}
    }
    else if (PyFloat_Check(value)) {
	mnumber mnval;
	mnval.type = MN_FLOAT;
	mnval.u.d = PyFloat_AsDouble(value);
	if (!setnparam(name, mnval)) {
	    PyErr_SetString(PyExc_RuntimeError,
		    "Failed to assign float parameter");
	    return NULL;
	}
    }
    else if (PyDict_Check(value)) {
	char **val, **valstart;
	PyObject *pkey, *pval;
	Py_ssize_t arrlen, pos = 0;

	arrlen = (2 * PyDict_Size(value) + 1) * sizeof(char *);
	val = (char **) zalloc(arrlen);
	valstart = val;

	while (PyDict_Next(value, &pos, &pkey, &pval)) {
	    if (!PyString_Check(pkey)) {
		PyErr_SetString(PyExc_TypeError,
			"Only string keys are allowed");
		FAIL_SETTING_ARRAY;
	    }
	    if (!PyString_Check(pval)) {
		PyErr_SetString(PyExc_TypeError,
			"Only string values are allowed");
		FAIL_SETTING_ARRAY;
	    }
	    *val++ = get_chars(pkey, zalloc);
	    *val++ = get_chars(pval, zalloc);
	}
	*val = NULL;

	if (!sethparam(name, valstart)) {
	    PyErr_SetString(PyExc_RuntimeError, "Failed to set hash");
	    return NULL;
	}
    }
    /* Python's list have no faster shortcut methods like PyDict_Next above
     * thus using more abstract protocol */
    else if (PySequence_Check(value)) {
	char **ss = get_chars_array(value, zalloc);

	if (!ss)
	    return NULL;

	if (!setaparam(name, ss)) {
	    PyErr_SetString(PyExc_RuntimeError, "Failed to set array");
	    return NULL;
	}
    }
    else if (value == Py_None) {
	unsetparam(name);
	if (errflag) {
	    PyErr_SetString(PyExc_RuntimeError, "Failed to delete parameter");
	    return NULL;
	}
    }
    else {
	PyErr_SetString(PyExc_TypeError,
		"Cannot assign value of the given type");
	return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *
ZshExitCode(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    return PyInt_FromLong((long) lastval);
}

static PyObject *
ZshColumns(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    return PyInt_FromLong((long) zterm_columns);
}

static PyObject *
ZshLines(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    return PyInt_FromLong((long) zterm_lines);
}

static PyObject *
ZshSubshell(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    return PyInt_FromLong((long) zsh_subshell);
}

static PyObject *
ZshPipeStatus(UNUSED(PyObject *self), UNUSED(PyObject *args))
{
    size_t i = 0;
    PyObject *r = PyList_New(numpipestats);
    PyObject *num;

    while (i < numpipestats) {
	if (!(num = PyInt_FromLong(pipestats[i]))) {
	    Py_DECREF(r);
	    return NULL;
	}
	if (PyList_SetItem(r, i, num) == -1) {
	    Py_DECREF(r);
	    return NULL;
	}
	i++;
    }
    return r;
}

static void
free_sp(struct specialparam *sp)
{
    if (sp->prev)
	sp->prev->next = sp->next;
    else
	first_assigned_param = sp->next;

    if (sp->next)
	sp->next->prev = sp->prev;
    else
	last_assigned_param = sp->prev;

    zsfree(sp->name);
    PyMem_Free(sp);
}

static void
unset_special_parameter(struct special_data *data)
{
    Py_DECREF(data->obj);
    free_sp(data->sp);
    PyMem_Free(data);
}


#define ZFAIL(errargs, failval) \
    PyErr_PrintEx(0); \
    PyErr_Clear(); \
    PYTHON_FINISH; \
    zerr errargs; \
    return failval

#define ZFAIL_NOFINISH(errargs, failval) \
    PyErr_PrintEx(0); \
    PyErr_Clear(); \
    zerr errargs; \
    return failval

struct sh_keyobj_data {
    PyObject *obj;
    PyObject *keyobj;
};

struct sh_key_data {
    PyObject *obj;
    char *key;
};

static char *
get_sh_item_value(PyObject *obj, PyObject *keyobj)
{
    PyObject *valobj, *string;
    char *str;

    if (!(valobj = PyObject_GetItem(obj, keyobj))) {
	if (PyErr_Occurred()) {
	    /* Expected result: key not found */
	    if (PyErr_ExceptionMatches(PyExc_KeyError)) {
		PyErr_Clear();
		return dupstring("");
	    }
	    /* Unexpected result: unknown exception */
	    else
		ZFAIL_NOFINISH(("Failed to get value object"), dupstring(""));
	}
	else
	    return dupstring("");
    }

    if (!(string = PyObject_Str(valobj))) {
	Py_DECREF(valobj);
	ZFAIL_NOFINISH(("Failed to get value string object"), dupstring(""));
    }

    if (!(str = get_chars(string, zhalloc))) {
	Py_DECREF(valobj);
	Py_DECREF(string);
	ZFAIL_NOFINISH(("Failed to get string from value string object"),
		dupstring(""));
    }
    Py_DECREF(string);
    Py_DECREF(valobj);
    return str;
}

static char *
get_sh_keyobj_value(Param pm)
{
    struct sh_keyobj_data *sh_kodata = (struct sh_keyobj_data *) pm->u.data;
    return get_sh_item_value(sh_kodata->obj, sh_kodata->keyobj);
}

static char *
get_sh_key_value(Param pm)
{
    char *r, *key;
    PyObject *keyobj, *obj;
    struct sh_key_data *sh_kdata = (struct sh_key_data *) pm->u.data;

    PYTHON_INIT(dupstring(""));

    obj = sh_kdata->obj;
    key = sh_kdata->key;

    if (!(keyobj = get_string(key))) {
	ZFAIL(("Failed to create key string object for key \"%s\"", key),
		dupstring(""));
    }
    r = get_sh_item_value(obj, keyobj);
    Py_DECREF(keyobj);

    PYTHON_FINISH;
    return r;
}

static void
set_sh_item_value(PyObject *obj, PyObject *keyobj, char *val)
{
    PyObject *valobj;

    if (!(valobj = get_string(val))) {
	ZFAIL_NOFINISH(("Failed to create value string object"), );
    }

    if (PyObject_SetItem(obj, keyobj, valobj) == -1) {
	Py_DECREF(valobj);
	ZFAIL_NOFINISH(("Failed to set object to \"%s\"", val), );
    }
    Py_DECREF(valobj);
}

static void
set_sh_key_value(Param pm, char *val)
{
    PyObject *obj, *keyobj;
    char *key;
    struct sh_key_data *sh_kdata = (struct sh_key_data *) pm->u.data;

    PYTHON_INIT();

    obj = sh_kdata->obj;
    key = sh_kdata->key;

    if (!(keyobj = get_string(key))) {
	ZFAIL(("Failed to create key string object for key \"%s\"", key), );
    }
    set_sh_item_value(obj, keyobj, val);
    Py_DECREF(keyobj);

    PYTHON_FINISH;
}

static void
set_sh_keyobj_value(Param pm, char *val)
{
    struct sh_keyobj_data *sh_kodata = (struct sh_keyobj_data *) pm->u.data;
    set_sh_item_value(sh_kodata->obj, sh_kodata->keyobj, val);
}

static struct gsu_scalar sh_keyobj_gsu =
{get_sh_keyobj_value, set_sh_keyobj_value, nullunsetfn};
static struct gsu_scalar sh_key_gsu =
{get_sh_key_value, set_sh_key_value, nullunsetfn};

static HashNode
get_sh_item(HashTable ht, const char *key)
{
    PyObject *obj = ((struct obj_hash_node *) (*ht->nodes))->obj;
    char *str;
    Param pm;
    struct sh_key_data *sh_kdata;

    PYTHON_INIT(NULL);

    pm = (Param) hcalloc(sizeof(struct param));
    pm->node.nam = dupstring(key);
    pm->node.flags = PM_SCALAR;
    pm->gsu.s = &sh_key_gsu;

    sh_kdata = (struct sh_key_data *) hcalloc(sizeof(struct sh_key_data) * 1);
    sh_kdata->obj = obj;
    sh_kdata->key = dupstring(key);

    pm->u.data = (void *) sh_kdata;

    PYTHON_FINISH;

    return &pm->node;
}

static void
scan_special_hash(HashTable ht, ScanFunc func, int flags)
{
    PyObject *obj = ((struct obj_hash_node *) (*ht->nodes))->obj;
    PyObject *iter, *keyobj;
    HashNode hn;
    struct param pm;

    memset((void *) &pm, 0, sizeof(struct param));
    pm.node.flags = PM_SCALAR;
    pm.gsu.s = &sh_keyobj_gsu;

    PYTHON_INIT();

    if (!(iter = PyObject_GetIter(obj))) {
	ZFAIL(("Failed to get iterator"), );
    }

    while ((keyobj = PyIter_Next(iter))) {
	PyObject *string;
	char *str;
	struct sh_keyobj_data sh_kodata;

	if (!(string = PyObject_Str(keyobj))) {
	    Py_DECREF(iter);
	    Py_DECREF(keyobj);
	    ZFAIL(("Failed to convert key to string object"), );
	}

	if (!(str = get_chars(string, zhalloc))) {
	    Py_DECREF(string);
	    Py_DECREF(iter);
	    Py_DECREF(keyobj);
	    ZFAIL(("Failed to get string from key string object"), );
	}
	Py_DECREF(string);
	pm.node.nam = str;

	sh_kodata.obj = obj;
	sh_kodata.keyobj = keyobj;
	pm.u.data = (void *) &sh_kodata;

	func(&pm.node, flags);

	Py_DECREF(keyobj);
    }
    Py_DECREF(iter);

    PYTHON_FINISH;
}

static char *
get_special_string(Param pm)
{
    PyObject *robj;
    char *r;

    PYTHON_INIT(dupstring(""));

    if (!(robj = PyObject_Str(((struct special_data *) pm->u.data)->obj))) {
	ZFAIL(("Failed to create string object for parameter %s",
		    pm->node.nam), dupstring(""));
    }

    if (!(r = get_chars(robj, zhalloc))) {
	ZFAIL(("Failed to transform value for parameter %s", pm->node.nam),
		dupstring(""));
    }

    Py_DECREF(robj);

    PYTHON_FINISH;

    return r;
}

static zlong
get_special_integer(Param pm)
{
    PyObject *robj;
    zlong r;

    PYTHON_INIT(0);

    if (!(robj = PyNumber_Int(((struct special_data *) pm->u.data)->obj))) {
	ZFAIL(("Failed to create int object for parameter %s", pm->node.nam),
		0);
    }

    r = PyInt_AsLong(robj);

    Py_DECREF(robj);

    PYTHON_FINISH;

    return r;
}

static double
get_special_float(Param pm)
{
    PyObject *robj;
    float r;

    PYTHON_INIT(0.0);

    if (!(robj = PyNumber_Float(((struct special_data *) pm->u.data)->obj))) {
	ZFAIL(("Failed to create float object for parameter %s", pm->node.nam),
		0);
    }

    r = PyFloat_AsDouble(robj);

    Py_DECREF(robj);

    PYTHON_FINISH;

    return r;
}

static char **
get_special_array(Param pm)
{
    char **r;

    PYTHON_INIT(hcalloc(sizeof(char **)));

    if (!(r = get_chars_array(((struct special_data *) pm->u.data)->obj,
		    zhalloc))) {
	ZFAIL(("Failed to create array object for parameter %s", pm->node.nam),
		hcalloc(sizeof(char **)));
    }

    PYTHON_FINISH;

    return r;
}

static void
set_special_string(Param pm, char *val)
{
    PyObject *r, *args;

    PYTHON_INIT();

    if (!val) {
	unset_special_parameter((struct special_data *) pm->u.data);

	PYTHON_FINISH;
	return;
    }

    args = Py_BuildValue("(O&)", get_string, val);
    r = PyObject_CallObject(((struct special_data *) pm->u.data)->obj, args);
    Py_DECREF(args);
    if (!r) {
	PyErr_PrintEx(0);
	zerr("Failed to assign value for string parameter %s", pm->node.nam);
	PYTHON_FINISH;
	return;
    }
    Py_DECREF(r);

    PYTHON_FINISH;
}

static void
set_special_integer(Param pm, zlong val)
{
    PyObject *r, *args;

    PYTHON_INIT();

    args = Py_BuildValue("(L)", (long long) val);
    r = PyObject_CallObject(((struct special_data *) pm->u.data)->obj, args);
    Py_DECREF(args);
    if (!r) {
	PyErr_PrintEx(0);
	zerr("Failed to assign value for integer parameter %s", pm->node.nam);
	PYTHON_FINISH;
	return;
    }
    Py_DECREF(r);

    PYTHON_FINISH;
}

static void
set_special_float(Param pm, double val)
{
    PyObject *r, *args;

    PYTHON_INIT();

    args = Py_BuildValue("(d)", val);
    r = PyObject_CallObject(((struct special_data *) pm->u.data)->obj, args);
    Py_DECREF(args);
    if (!r) {
	PyErr_PrintEx(0);
	zerr("Failed to assign value for float parameter %s", pm->node.nam);
	PYTHON_FINISH;
	return;
    }
    Py_DECREF(r);

    PYTHON_FINISH;
}

static void
set_special_array(Param pm, char **val)
{
    PyObject *r, *args;

    PYTHON_INIT();

    if (!val) {
	unset_special_parameter((struct special_data *) pm->u.data);

	PYTHON_FINISH;
	return;
    }

    args = Py_BuildValue("(O&)", get_array, val);
    r = PyObject_CallObject(((struct special_data *) pm->u.data)->obj, args);
    Py_DECREF(args);
    if (!r) {
	PyErr_PrintEx(0);
	zerr("Failed to assign value for array parameter %s", pm->node.nam);
	PYTHON_FINISH;
	return;
    }
    Py_DECREF(r);

    PYTHON_FINISH;
}

static void
set_special_hash(Param pm, HashTable ht)
{
    int i;
    HashNode hn;
    PyObject *obj = ((struct obj_hash_node *) (*pm->u.hash->nodes))->obj;
    PyObject *keys, *iter, *keyobj;

    if (pm->u.hash == ht)
	return;

    PYTHON_INIT();

    if (!ht) {
	struct specialparam *sp =
	    ((struct obj_hash_node *) (*pm->u.hash->nodes))->sp;
	free_sp(sp);
	Py_DECREF(obj);
	PYTHON_FINISH;
	return;
    }

    /* Can't use PyObject_GetIter on the object itself: it fails if object is
     * being modified */
    if (!(keys = PyMapping_Keys(obj))) {
	ZFAIL(("Failed to get object keys"), );
    }
    if (!(iter = PyObject_GetIter(keys))) {
	ZFAIL(("Failed to get keys iterator"), );
    }
    while ((keyobj = PyIter_Next(iter))) {
	if (PyMapping_DelItem(obj, keyobj) == -1) {
	    ZFAIL(("Failed to delete some key"), );
	}
	Py_DECREF(keyobj);
    }
    Py_DECREF(iter);
    Py_DECREF(keys);

    for (i = 0; i < ht->hsize; i++)
	for (hn = ht->nodes[i]; hn; hn = hn->next) {
	    struct value v;
	    char *val;
	    PyObject *valobj, *keyobj;

	    v.isarr = v.flags = v.start = 0;
	    v.end = -1;
	    v.arr = NULL;
	    v.pm = (Param) hn;

	    val = getstrvalue(&v);
	    if (!val) {
		ZFAIL(("Failed to get string value"), );
	    }

	    if (!(valobj = get_string(val))) {
		ZFAIL(("Failed to convert value \"%s\" to string object "
			    "while processing key", val, hn->nam), );
	    }
	    if (!(keyobj = get_string(hn->nam))) {
		Py_DECREF(valobj);
		ZFAIL(("Failed to convert key \"%s\" to string object",
			    hn->nam), );
	    }

	    if (PyObject_SetItem(obj, keyobj, valobj) == -1) {
		Py_DECREF(valobj);
		Py_DECREF(keyobj);
		ZFAIL(("Failed to set key %s", hn->nam), );
	    }

	    Py_DECREF(valobj);
	    Py_DECREF(keyobj);
	}
    PYTHON_FINISH;
}

static void
unsetfn(Param pm, int exp)
{
    unset_special_parameter((struct special_data *) pm->u.data);
    stdunsetfn(pm, exp);
}

static void
free_sh_node(HashNode nodeptr)
{
    /* Param is obtained from get_sh_item */
    Param pm = (Param) nodeptr;
    struct sh_key_data *sh_kdata = (struct sh_key_data *) pm->u.data;
    PyObject *keyobj;

    PYTHON_INIT();

    if (!(keyobj = get_string(sh_kdata->key))) {
	ZFAIL(("While unsetting key %s of parameter %s failed to get "
		    "key string object", sh_kdata->key, pm->node.nam), );
    }
    if (PyMapping_DelItem(sh_kdata->obj, keyobj) == -1) {
	Py_DECREF(keyobj);
	ZFAIL(("Failed to delete key %s of parameter %s",
		    sh_kdata->key, pm->node.nam), );
    }
    Py_DECREF(keyobj);

    PYTHON_FINISH;
}

static const struct gsu_scalar special_string_gsu =
{get_special_string, set_special_string, stdunsetfn};
static const struct gsu_integer special_integer_gsu =
{get_special_integer, set_special_integer, unsetfn};
static const struct gsu_float special_float_gsu =
{get_special_float, set_special_float, unsetfn};
static const struct gsu_array special_array_gsu =
{get_special_array, set_special_array, stdunsetfn};
static const struct gsu_hash special_hash_gsu =
{hashgetfn, set_special_hash, stdunsetfn};

static int
check_special_name(char *name)
{
    /* Needing strncasecmp, but the one that ignores locale */
    if (!(         (name[0] == 'z' || name[0] == 'Z')
		&& (name[1] == 'p' || name[1] == 'P')
		&& (name[2] == 'y' || name[2] == 'Y')
		&& (name[3] == 't' || name[3] == 'T')
		&& (name[4] == 'h' || name[4] == 'H')
		&& (name[5] == 'o' || name[5] == 'O')
		&& (name[6] == 'n' || name[6] == 'N')
		&& (name[7] != '\0')
       ) || !isident(name))
    {
	PyErr_SetString(PyExc_KeyError, "Invalid special identifier: "
		"it must be a valid variable name starting with "
		"\"zpython\" (ignoring case) and containing at least one more "
		"character");
	return 1;
    }
    return 0;
}

static PyObject *
set_special_parameter(PyObject *args, int type)
{
    char *name;
    PyObject *obj;
    Param pm;
    int flags = type;
    struct special_data *data;
    struct specialparam *sp;

    if (!PyArg_ParseTuple(args, "sO", &name, &obj))
	return NULL;

    if (check_special_name(name))
	return NULL;

    switch (type) {
    case PM_SCALAR:
	break;
    case PM_INTEGER:
    case PM_EFLOAT:
    case PM_FFLOAT:
	if (!PyNumber_Check(obj)) {
	    PyErr_SetString(PyExc_TypeError,
		    "Object must implement numeric protocol");
	    return NULL;
	}
	break;
    case PM_ARRAY:
	if (!PySequence_Check(obj)) {
	    PyErr_SetString(PyExc_TypeError,
		    "Object must implement sequence protocol");
	    return NULL;
	}
	break;
    case PM_HASHED:
	if (!PyMapping_Check(obj)) {
	    PyErr_SetString(PyExc_TypeError,
		    "Object must implement mapping protocol");
	    return NULL;
	}
	break;
    }

    if (type == PM_HASHED) {
	if (!(pm = createspecialhash(name, get_sh_item,
				    scan_special_hash, flags))) {
	    PyErr_SetString(PyExc_RuntimeError, "Failed to create parameter");
	    return NULL;
	}
    }
    else {
	if (!(pm = createparam(name, flags))) {
	    PyErr_SetString(PyExc_RuntimeError, "Failed to create parameter");
	    return NULL;
	}
    }

    sp = PyMem_New(struct specialparam, 1);
    sp->prev = last_assigned_param;
    if (last_assigned_param)
	last_assigned_param->next = sp;
    else
	first_assigned_param = sp;
    last_assigned_param = sp;
    sp->next = NULL;
    sp->name = ztrdup(name);
    sp->pm = pm;

    if (type != PM_HASHED) {
	data = PyMem_New(struct special_data, 1);
	data->sp = sp;
	data->obj = obj;
	Py_INCREF(obj);
	pm->u.data = data;
    }

    pm->level = 0;

    switch (type) {
    case PM_SCALAR:
	pm->gsu.s = &special_string_gsu;
	break;
    case PM_INTEGER:
	pm->gsu.i = &special_integer_gsu;
	break;
    case PM_EFLOAT:
    case PM_FFLOAT:
	pm->gsu.f = &special_float_gsu;
	break;
    case PM_ARRAY:
	pm->gsu.a = &special_array_gsu;
	break;
    case PM_HASHED:
	{
	    struct obj_hash_node *ohn;
	    HashTable ht = pm->u.hash;
	    ohn = PyMem_New(struct obj_hash_node, 1);
	    ohn->nam = ztrdup("obj");
	    ohn->obj = obj;
	    Py_INCREF(obj);
	    ohn->sp = sp;
	    zfree(ht->nodes, ht->hsize * sizeof(HashNode));
	    ht->nodes = (HashNode *) zshcalloc(1 * sizeof(HashNode));
	    ht->hsize = 1;
	    *ht->nodes = (struct hashnode *) ohn;
	    pm->gsu.h = &special_hash_gsu;
	    ht->freenode = free_sh_node;
	    break;
	}
    }

    Py_RETURN_NONE;
}

static PyObject *
ZshSetMagicString(UNUSED(PyObject *self), PyObject *args)
{
    return set_special_parameter(args, PM_SCALAR);
}

static PyObject *
ZshSetMagicInteger(UNUSED(PyObject *self), PyObject *args)
{
    return set_special_parameter(args, PM_INTEGER);
}

static PyObject *
ZshSetMagicFloat(UNUSED(PyObject *self), PyObject *args)
{
    return set_special_parameter(args, PM_EFLOAT);
}

static PyObject *
ZshSetMagicArray(UNUSED(PyObject *self), PyObject *args)
{
    return set_special_parameter(args, PM_ARRAY);
}

static PyObject *
ZshSetMagicHash(UNUSED(PyObject *self), PyObject *args)
{
    return set_special_parameter(args, PM_HASHED);
}

static struct PyMethodDef ZshMethods[] = {
    {"eval", ZshEval, 1, "Evaluate command in current shell context",},
    {"last_exit_code", ZshExitCode, 0, "Get last exit code. Returns an int"},
    {"pipestatus", ZshPipeStatus, 0, "Get last pipe status. Returns a list of int"},
    {"columns", ZshColumns, 0, "Get number of columns. Returns an int"},
    {"lines", ZshLines, 0, "Get number of lines. Returns an int"},
    {"subshell", ZshSubshell, 0, "Get subshell recursion depth. Returns an int"},
    {"getvalue", ZshGetValue, 1,
	"Get parameter value. Return types:\n"
	"  str              for scalars\n"
	"  long             for integer numbers\n"
	"  float            for floating-point numbers\n"
	"  list [str]       for array parameters\n"
	"  dict {str : str} for associative arrays\n"
	"Throws KeyError   if identifier is invalid,\n"
	"       IndexError if parameter was not found\n"
    },
    {"setvalue", ZshSetValue, 2,
	"Set parameter value. Use None to unset. Supported objects and corresponding\n"
	"zsh parameter types:\n"
	"  str               sets string scalars\n"
	"  long or int       sets integer numbers\n"
	"  float             sets floating-point numbers. Output is in scientific notation\n"
	"  sequence of str   sets array parameters (sequence = anything implementing\n"
	"                    sequence protocol)\n"
	"  dict {str : str}  sets hashes\n"
	"Throws KeyError     if identifier is invalid,\n"
	"       RuntimeError if zsh set?param/unsetparam function failed,\n"
	"       ValueError   if sequence item or dictionary key or value are not str\n"
	"                       or sequence size is not known."},
    {"set_special_string", ZshSetMagicString, 2,
	"Define scalar (string) parameter.\n"
	"First argument is parameter name, it must start with zpython (case is ignored).\n"
	"  Parameter with given name must not exist.\n"
	"Second argument is value object. Its __str__ method will be used to get\n"
	"  resulting string when parameter is accessed in zsh, __call__ method will be used\n"
	"  to set value. If object is not callable then parameter will be considered readonly"},
    {"set_special_integer", ZshSetMagicInteger, 2,
	"Define integer parameter.\n"
	"First argument is parameter name, it must start with zpython (case is ignored).\n"
	"  Parameter with given name must not exist.\n"
	"Second argument is value object. It will be coerced to long integer,\n"
	"  __call__ method will be used to set value. If object is not callable\n"
	"  then parameter will be considered readonly"},
    {"set_special_float", ZshSetMagicFloat, 2,
	"Define floating point parameter.\n"
	"First argument is parameter name, it must start with zpython (case is ignored).\n"
	"  Parameter with given name must not exist.\n"
	"Second argument is value object. It will be coerced to float,\n"
	"  __call__ method will be used to set value. If object is not callable\n"
	"  then parameter will be considered readonly"},
    {"set_special_array", ZshSetMagicArray, 2,
	"Define array parameter.\n"
	"First argument is parameter name, it must start with zpython (case is ignored).\n"
	"  Parameter with given name must not exist.\n"
	"Second argument is value object. It must implement sequence protocol,\n"
	"  each item in sequence must have str type, __call__ method will be used\n"
	"  to set value. If object is not callable then parameter will be\n"
	"  considered readonly"},
    {"set_special_hash", ZshSetMagicHash, 2,
	"Define hash parameter.\n"
	"First argument is parameter name, it must start with zpython (case is ignored).\n"
	"  Parameter with given name must not exist.\n"
	"Second argument is value object. It must implement mapping protocol,\n"
	"  it may be iterable (in this case iterator must return keys),\n"
	"  __getitem__ must be able to work with string objects,\n"
	"  each item must have str type.\n"
	"  __setitem__ will be used to set hash items"},
    {NULL, NULL, 0, NULL},
};

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
boot_(UNUSED(Module m))
{
    zpython_subshell = zsh_subshell;
    Py_Initialize();
    PyEval_InitThreads();
    Py_InitModule3("zsh", ZshMethods, (char *) NULL);
    globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    PYTHON_SAVE_THREAD;
    return 0;
}

/**/
int
cleanup_(Module m)
{
    if (Py_IsInitialized()) {
	struct specialparam *cur_sp = first_assigned_param;

	while (cur_sp) {
	    char *name = cur_sp->name;
	    Param pm = (Param) paramtab->getnode(paramtab, name);
	    struct specialparam *next_sp = cur_sp->next;

	    if (pm && pm != cur_sp->pm) {
		Param prevpm, searchpm;
		prevpm = pm;
		searchpm = pm->old;
		while (searchpm && searchpm != cur_sp->pm) {
		    prevpm = searchpm;
		    searchpm = searchpm->old;
		}
		if (searchpm) {
		    paramtab->removenode(paramtab, pm->node.nam);
		    prevpm->old = searchpm->old;
		    searchpm->old = pm;
		    paramtab->addnode(paramtab, searchpm->node.nam, searchpm);

		    pm = searchpm;
		}
		else {
		    pm = NULL;
		}
	    }
	    if (pm) {
		pm->node.flags = (pm->node.flags & ~PM_READONLY) | PM_REMOVABLE;
		unsetparam_pm(pm, 0, 1);
	    }
	    /* Memory was freed while unsetting parameter, thus need to save
	     * sp->next */
	    cur_sp = next_sp;
	}
	PYTHON_RESTORE_THREAD;
	Py_Finalize();
    }
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    return 0;
}
