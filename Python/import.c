/***********************************************************
Copyright 1991-1995 by Stichting Mathematisch Centrum, Amsterdam,
The Netherlands.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the names of Stichting Mathematisch
Centrum or CWI or Corporation for National Research Initiatives or
CNRI not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

While CWI is the initial source for this software, a modified version
is made available by the Corporation for National Research Initiatives
(CNRI) at the Internet address ftp://ftp.python.org.

STICHTING MATHEMATISCH CENTRUM AND CNRI DISCLAIM ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL STICHTING MATHEMATISCH
CENTRUM OR CNRI BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

/* Module definition and import implementation */

#include "Python.h"

#include "node.h"
#include "token.h"
#include "errcode.h"
#include "marshal.h"
#include "compile.h"
#include "eval.h"
#include "osdefs.h"
#include "importdl.h"
#ifdef macintosh
#include "macglue.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* We expect that stat exists on most systems.
   It's confirmed on Unix, Mac and Windows.
   If you don't have it, add #define DONT_HAVE_STAT to your config.h. */
#ifndef DONT_HAVE_STAT
#define HAVE_STAT

#include <sys/types.h>
#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#endif


extern long PyOS_GetLastModificationTime(); /* In getmtime.c */

/* Magic word to reject .pyc files generated by other Python versions */
/* Change for each incompatible change */
/* The value of CR and LF is incorporated so if you ever read or write
   a .pyc file in text mode the magic number will be wrong; also, the
   Apple MPW compiler swaps their values, botching string constants */
/* XXX Perhaps the magic number should be frozen and a version field
   added to the .pyc file header? */
/* New way to come up with the magic number: (YEAR-1995), MONTH, DAY */
#define MAGIC (20121 | ((long)'\r'<<16) | ((long)'\n'<<24))

/* See _PyImport_FixupExtension() below */
static PyObject *extensions = NULL;


/* Initialize things */

void
_PyImport_Init()
{
	if (Py_OptimizeFlag) {
		/* Replace ".pyc" with ".pyo" in import_filetab */
		struct filedescr *p;
		for (p = _PyImport_Filetab; p->suffix != NULL; p++) {
			if (strcmp(p->suffix, ".pyc") == 0)
				p->suffix = ".pyo";
		}
	}
}

void
_PyImport_Fini()
{
	Py_XDECREF(extensions);
	extensions = NULL;
}


/* Helper for sys */

PyObject *
PyImport_GetModuleDict()
{
	PyInterpreterState *interp = PyThreadState_Get()->interp;
	if (interp->modules == NULL)
		Py_FatalError("PyImport_GetModuleDict: no module dictionary!");
	return interp->modules;
}


/* Helper for PyImport_Cleanup */

static void
clear_carefully(d)
	PyObject *d;
{
	/* To make the execution order of destructors for global
	   objects a bit more predictable, we first zap all objects
	   whose name starts with a single underscore, before we clear
	   the entire dictionary.  We zap them by replacing them with
	   None, rather than deleting them from the dictionary, to
	   avoid rehashing the dictionary (to some extent). */

	int pos;
	PyObject *key, *value;

	pos = 0;
	while (PyDict_Next(d, &pos, &key, &value)) {
		if (value != Py_None && PyString_Check(key)) {
			char *s = PyString_AsString(key);
			if (s[0] == '_' && s[1] != '_')
				PyDict_SetItem(d, key, Py_None);
		}
	}
	
	PyDict_Clear(d);
}

/* Un-initialize things, as good as we can */

void
PyImport_Cleanup()
{
	PyInterpreterState *interp = PyThreadState_Get()->interp;
	PyObject *tmp = interp->modules;
	if (tmp != NULL) {
		int pos;
		PyObject *key, *value;
		interp->modules = NULL;
		pos = 0;
		while (PyDict_Next(tmp, &pos, &key, &value)) {
			if (PyModule_Check(value)) {
				PyObject *d = PyModule_GetDict(value);
				clear_carefully(d);
			}
		}
		PyDict_Clear(tmp);
		Py_DECREF(tmp);
	}
}


/* Helper for pythonrun.c -- return magic number */

long
PyImport_GetMagicNumber()
{
	return MAGIC;
}


/* Magic for extension modules (built-in as well as dynamically
   loaded).  To prevent initializing an extension module more than
   once, we keep a static dictionary 'extensions' keyed by module name
   (for built-in modules) or by filename (for dynamically loaded
   modules), containing these modules.  A copy od the module's
   dictionary is stored by calling _PyImport_FixupExtension()
   immediately after the module initialization function succeeds.  A
   copy can be retrieved from there by calling
   _PyImport_FindExtension(). */

PyObject *
_PyImport_FixupExtension(name, filename)
	char *name;
	char *filename;
{
	PyObject *modules, *mod, *dict, *copy;
	if (extensions == NULL) {
		extensions = PyDict_New();
		if (extensions == NULL)
			return NULL;
	}
	modules = PyImport_GetModuleDict();
	mod = PyDict_GetItemString(modules, name);
	if (mod == NULL || !PyModule_Check(mod)) {
		PyErr_Format(PyExc_SystemError,
		  "_PyImport_FixupExtension: module %.200s not loaded", name);
		return NULL;
	}
	dict = PyModule_GetDict(mod);
	if (dict == NULL)
		return NULL;
	copy = PyObject_CallMethod(dict, "copy", "");
	if (copy == NULL)
		return NULL;
	PyDict_SetItemString(extensions, filename, copy);
	Py_DECREF(copy);
	return copy;
}

PyObject *
_PyImport_FindExtension(name, filename)
	char *name;
	char *filename;
{
	PyObject *dict, *mod, *mdict, *result;
	if (extensions == NULL)
		return NULL;
	dict = PyDict_GetItemString(extensions, filename);
	if (dict == NULL)
		return NULL;
	mod = PyImport_AddModule(name);
	if (mod == NULL)
		return NULL;
	mdict = PyModule_GetDict(mod);
	if (mdict == NULL)
		return NULL;
	result = PyObject_CallMethod(mdict, "update", "O", dict);
	if (result == NULL)
		return NULL;
	Py_DECREF(result);
	if (Py_VerboseFlag)
		fprintf(stderr, "import %s # previously loaded (%s)\n",
			name, filename);
	return mod;
}


/* Get the module object corresponding to a module name.
   First check the modules dictionary if there's one there,
   if not, create a new one and insert in in the modules dictionary.
   Because the former action is most common, THIS DOES NOT RETURN A
   'NEW' REFERENCE! */

PyObject *
PyImport_AddModule(name)
	char *name;
{
	PyObject *modules = PyImport_GetModuleDict();
	PyObject *m;

	if ((m = PyDict_GetItemString(modules, name)) != NULL &&
	    PyModule_Check(m))
		return m;
	m = PyModule_New(name);
	if (m == NULL)
		return NULL;
	if (PyDict_SetItemString(modules, name, m) != 0) {
		Py_DECREF(m);
		return NULL;
	}
	Py_DECREF(m); /* Yes, it still exists, in modules! */

	return m;
}


/* Execute a code object in a module and return the module object
   WITH INCREMENTED REFERENCE COUNT */

PyObject *
PyImport_ExecCodeModule(name, co)
	char *name;
	PyObject *co;
{
	PyObject *modules = PyImport_GetModuleDict();
	PyObject *m, *d, *v;

	m = PyImport_AddModule(name);
	if (m == NULL)
		return NULL;
	d = PyModule_GetDict(m);
	if (PyDict_GetItemString(d, "__builtins__") == NULL) {
		if (PyDict_SetItemString(d, "__builtins__",
					 PyEval_GetBuiltins()) != 0)
			return NULL;
	}
	/* Remember the filename as the __file__ attribute */
	if (PyDict_SetItemString(d, "__file__",
				 ((PyCodeObject *)co)->co_filename) != 0)
		PyErr_Clear(); /* Not important enough to report */
	v = PyEval_EvalCode((PyCodeObject *)co, d, d);
	if (v == NULL)
		return NULL;
	Py_DECREF(v);

	if ((m = PyDict_GetItemString(modules, name)) == NULL) {
		PyErr_Format(PyExc_ImportError,
			     "Loaded module %.200s not found in sys.modules",
			     name);
		return NULL;
	}

	Py_INCREF(m);

	return m;
}


/* Given a pathname for a Python source file, fill a buffer with the
   pathname for the corresponding compiled file.  Return the pathname
   for the compiled file, or NULL if there's no space in the buffer.
   Doesn't set an exception. */

static char *
make_compiled_pathname(pathname, buf, buflen)
	char *pathname;
	char *buf;
	int buflen;
{
	int len;

	len = strlen(pathname);
	if (len+2 > buflen)
		return NULL;
	strcpy(buf, pathname);
	strcpy(buf+len, Py_OptimizeFlag ? "o" : "c");

	return buf;
}


/* Given a pathname for a Python source file, its time of last
   modification, and a pathname for a compiled file, check whether the
   compiled file represents the same version of the source.  If so,
   return a FILE pointer for the compiled file, positioned just after
   the header; if not, return NULL.
   Doesn't set an exception. */

static FILE *
check_compiled_module(pathname, mtime, cpathname)
	char *pathname;
	long mtime;
	char *cpathname;
{
	FILE *fp;
	long magic;
	long pyc_mtime;

	fp = fopen(cpathname, "rb");
	if (fp == NULL)
		return NULL;
	magic = PyMarshal_ReadLongFromFile(fp);
	if (magic != MAGIC) {
		if (Py_VerboseFlag)
			fprintf(stderr, "# %s has bad magic\n", cpathname);
		fclose(fp);
		return NULL;
	}
	pyc_mtime = PyMarshal_ReadLongFromFile(fp);
	if (pyc_mtime != mtime) {
		if (Py_VerboseFlag)
			fprintf(stderr, "# %s has bad mtime\n", cpathname);
		fclose(fp);
		return NULL;
	}
	if (Py_VerboseFlag)
		fprintf(stderr, "# %s matches %s\n", cpathname, pathname);
	return fp;
}


/* Read a code object from a file and check it for validity */

static PyCodeObject *
read_compiled_module(cpathname, fp)
	char *cpathname;
	FILE *fp;
{
	PyObject *co;

	co = PyMarshal_ReadObjectFromFile(fp);
	/* Ugly: rd_object() may return NULL with or without error */
	if (co == NULL || !PyCode_Check(co)) {
		if (!PyErr_Occurred())
			PyErr_Format(PyExc_ImportError,
			    "Non-code object in %.200s", cpathname);
		Py_XDECREF(co);
		return NULL;
	}
	return (PyCodeObject *)co;
}


/* Load a module from a compiled file, execute it, and return its
   module object WITH INCREMENTED REFERENCE COUNT */

static PyObject *
load_compiled_module(name, cpathname, fp)
	char *name;
	char *cpathname;
	FILE *fp;
{
	long magic;
	PyCodeObject *co;
	PyObject *m;

	magic = PyMarshal_ReadLongFromFile(fp);
	if (magic != MAGIC) {
		PyErr_Format(PyExc_ImportError,
			     "Bad magic number in %.200s", cpathname);
		return NULL;
	}
	(void) PyMarshal_ReadLongFromFile(fp);
	co = read_compiled_module(cpathname, fp);
	if (co == NULL)
		return NULL;
	if (Py_VerboseFlag)
		fprintf(stderr, "import %s # precompiled from %s\n",
			name, cpathname);
	m = PyImport_ExecCodeModule(name, (PyObject *)co);
	Py_DECREF(co);

	return m;
}

/* Parse a source file and return the corresponding code object */

static PyCodeObject *
parse_source_module(pathname, fp)
	char *pathname;
	FILE *fp;
{
	PyCodeObject *co;
	node *n;

	n = PyParser_SimpleParseFile(fp, pathname, Py_file_input);
	if (n == NULL)
		return NULL;
	co = PyNode_Compile(n, pathname);
	PyNode_Free(n);

	return co;
}


/* Write a compiled module to a file, placing the time of last
   modification of its source into the header.
   Errors are ignored, if a write error occurs an attempt is made to
   remove the file. */

static void
write_compiled_module(co, cpathname, mtime)
	PyCodeObject *co;
	char *cpathname;
	long mtime;
{
	FILE *fp;

	fp = fopen(cpathname, "wb");
	if (fp == NULL) {
		if (Py_VerboseFlag)
			fprintf(stderr,
				"# can't create %s\n", cpathname);
		return;
	}
	PyMarshal_WriteLongToFile(MAGIC, fp);
	/* First write a 0 for mtime */
	PyMarshal_WriteLongToFile(0L, fp);
	PyMarshal_WriteObjectToFile((PyObject *)co, fp);
	if (ferror(fp)) {
		if (Py_VerboseFlag)
			fprintf(stderr, "# can't write %s\n", cpathname);
		/* Don't keep partial file */
		fclose(fp);
		(void) unlink(cpathname);
		return;
	}
	/* Now write the true mtime */
	fseek(fp, 4L, 0);
	PyMarshal_WriteLongToFile(mtime, fp);
	fflush(fp);
	fclose(fp);
	if (Py_VerboseFlag)
		fprintf(stderr, "# wrote %s\n", cpathname);
#ifdef macintosh
	setfiletype(cpathname, 'Pyth', 'PYC ');
#endif
}


/* Load a source module from a given file and return its module
   object WITH INCREMENTED REFERENCE COUNT.  If there's a matching
   byte-compiled file, use that instead. */

static PyObject *
load_source_module(name, pathname, fp)
	char *name;
	char *pathname;
	FILE *fp;
{
	long mtime;
	FILE *fpc;
	char buf[MAXPATHLEN+1];
	char *cpathname;
	PyCodeObject *co;
	PyObject *m;

	mtime = PyOS_GetLastModificationTime(pathname, fp);
	cpathname = make_compiled_pathname(pathname, buf, MAXPATHLEN+1);
	if (cpathname != NULL &&
	    (fpc = check_compiled_module(pathname, mtime, cpathname))) {
		co = read_compiled_module(cpathname, fpc);
		fclose(fpc);
		if (co == NULL)
			return NULL;
		if (Py_VerboseFlag)
			fprintf(stderr, "import %s # precompiled from %s\n",
				name, cpathname);
	}
	else {
		co = parse_source_module(pathname, fp);
		if (co == NULL)
			return NULL;
		if (Py_VerboseFlag)
			fprintf(stderr, "import %s # from %s\n",
				name, pathname);
		write_compiled_module(co, cpathname, mtime);
	}
	m = PyImport_ExecCodeModule(name, (PyObject *)co);
	Py_DECREF(co);

	return m;
}


/* Forward */
static PyObject *load_module Py_PROTO((char *, FILE *, char *, int));
static struct filedescr *find_module Py_PROTO((char *, PyObject *,
					       char *, int, FILE **));

/* Load a package and return its module object WITH INCREMENTED
   REFERENCE COUNT */

static PyObject *
load_package(name, pathname)
	char *name;
	char *pathname;
{
	PyObject *m, *d, *file, *path;
	int err;
	char buf[MAXPATHLEN+1];
	FILE *fp = NULL;
	struct filedescr *fdp;

	m = PyImport_AddModule(name);
	if (m == NULL)
		return NULL;
	d = PyModule_GetDict(m);
	file = PyString_FromString(pathname);
	if (file == NULL)
		return NULL;
	path = Py_BuildValue("[O]", file);
	if (path == NULL) {
		Py_DECREF(file);
		return NULL;
	}
	err = PyDict_SetItemString(d, "__file__", file);
	if (err == 0)
		err = PyDict_SetItemString(d, "__path__", path);
	if (err != 0) {
		m = NULL;
		goto cleanup;
	}
	buf[0] = '\0';
	fdp = find_module("__init__", path, buf, sizeof(buf), &fp);
	if (fdp == NULL) {
		if (PyErr_ExceptionMatches(PyExc_ImportError)) {
			PyErr_Clear();
		}
		else
			m = NULL;
		goto cleanup;
	}
	m = load_module(name, fp, buf, fdp->type);
	if (fp != NULL)
		fclose(fp);
  cleanup:
	Py_XINCREF(m);
	Py_XDECREF(path);
	Py_XDECREF(file);
	return m;
}


/* Helper to test for built-in module */

static int
is_builtin(name)
	char *name;
{
	int i;
	for (i = 0; _PyImport_Inittab[i].name != NULL; i++) {
		if (strcmp(name, _PyImport_Inittab[i].name) == 0) {
			if (_PyImport_Inittab[i].initfunc == NULL)
				return -1;
			else
				return 1;
		}
	}
	return 0;
}

/* Helper to test for frozen module */

static int
is_frozen(name)
	char *name;
{
	struct _frozen *p;
	for (p = PyImport_FrozenModules; ; p++) {
		if (p->name == NULL)
			break;
		if (strcmp(p->name, name) == 0)
			return 1;
	}
	return 0;
}


/* Search the path (default sys.path) for a module.  Return the
   corresponding filedescr struct, and (via return arguments) the
   pathname and an open file.  Return NULL if the module is not found. */

#ifdef MS_COREDLL
extern FILE *PyWin_FindRegisteredModule();
#endif

static struct filedescr *
find_module(name, path, buf, buflen, p_fp)
	char *name;
	PyObject *path;
	/* Output parameters: */
	char *buf;
	int buflen;
	FILE **p_fp;
{
	int i, npath, len, namelen;
	struct filedescr *fdp = NULL;
	FILE *fp = NULL;
	struct stat statbuf;

	if (path == NULL) {
		if (is_builtin(name)) {
			static struct filedescr fd = {"", "", C_BUILTIN};
			return &fd;
		}
		if (is_frozen(name)) {
			static struct filedescr fd = {"", "", PY_FROZEN};
			return &fd;
		}

#ifdef MS_COREDLL
		fp = PyWin_FindRegisteredModule(name, &fdp, buf, buflen);
		if (fp != NULL) {
			*p_fp = fp;
			return fdp;
		}
#endif
	}


	if (path == NULL)
		path = PySys_GetObject("path");
	if (path == NULL || !PyList_Check(path)) {
		PyErr_SetString(PyExc_ImportError,
			   "sys.path must be a list of directory names");
		return NULL;
	}
	npath = PyList_Size(path);
	namelen = strlen(name);
	for (i = 0; i < npath; i++) {
		PyObject *v = PyList_GetItem(path, i);
		if (!PyString_Check(v))
			continue;
		len = PyString_Size(v);
		if (len + 2 + namelen + MAXSUFFIXSIZE >= buflen)
			continue; /* Too long */
		strcpy(buf, PyString_AsString(v));
		if ((int)strlen(buf) != len)
			continue; /* v contains '\0' */
#ifdef macintosh
#ifdef INTERN_STRINGS
		/* 
		** Speedup: each sys.path item is interned, and
		** FindResourceModule remembers which items refer to
		** folders (so we don't have to bother trying to look
		** into them for resources). 
		*/
		PyString_InternInPlace(&PyList_GET_ITEM(path, i));
		v = PyList_GET_ITEM(path, i);
#endif
		if (PyMac_FindResourceModule((PyStringObject *)v, name, buf)) {
			static struct filedescr resfiledescr =
				{"", "", PY_RESOURCE};
			
			return &resfiledescr;
		}
#endif
		if (len > 0 && buf[len-1] != SEP
#ifdef ALTSEP
		    && buf[len-1] != ALTSEP
#endif
		    )
			buf[len++] = SEP;
#ifdef macintosh
		fdp = PyMac_FindModuleExtension(buf, &len, name);
		if ( fdp )
			fp = fopen(buf, fdp->mode);
#else
#ifdef IMPORT_8x3_NAMES
		/* see if we are searching in directory dos_8x3 */
		if (len > 7 && !strncmp(buf + len - 8, "dos_8x3", 7)){
			int j;
			char ch;  /* limit name to 8 lower-case characters */
			for (j = 0; (ch = name[j]) && j < 8; j++)
				if (isupper(ch))
					buf[len++] = tolower(ch);
				else
					buf[len++] = ch;
		}
		else /* Not in dos_8x3, use the full name */
#endif
		{
			strcpy(buf+len, name);
			len += namelen;
		}
#ifdef HAVE_STAT
		if (stat(buf, &statbuf) == 0) {
			static struct filedescr fd = {"", "", PKG_DIRECTORY};
			if (S_ISDIR(statbuf.st_mode))
				return &fd;
		}
#else
		/* XXX How are you going to test for directories? */
#endif
		for (fdp = _PyImport_Filetab; fdp->suffix != NULL; fdp++) {
			strcpy(buf+len, fdp->suffix);
			if (Py_VerboseFlag > 1)
				fprintf(stderr, "# trying %s\n", buf);
			fp = fopen(buf, fdp->mode);
			if (fp != NULL)
				break;
		}
#endif /* !macintosh */
		if (fp != NULL)
			break;
	}
	if (fp == NULL) {
		PyErr_Format(PyExc_ImportError,
			     "No module named %.200s", name);
		return NULL;
	}

	*p_fp = fp;
	return fdp;
}


static int init_builtin Py_PROTO((char *)); /* Forward */

/* Load an external module using the default search path and return
   its module object WITH INCREMENTED REFERENCE COUNT */

static PyObject *
load_module(name, fp, buf, type)
	char *name;
	FILE *fp;
	char *buf;
	int type;
{
	PyObject *modules;
	PyObject *m;
	int err;

	/* First check that there's an open file (if we need one)  */
	switch (type) {
	case PY_SOURCE:
	case PY_COMPILED:
		if (fp == NULL) {
			PyErr_Format(PyExc_ValueError,
			   "file object required for import (type code %d)",
				     type);
			return NULL;
		}
	}

	switch (type) {

	case PY_SOURCE:
		m = load_source_module(name, buf, fp);
		break;

	case PY_COMPILED:
		m = load_compiled_module(name, buf, fp);
		break;

	case C_EXTENSION:
		m = _PyImport_LoadDynamicModule(name, buf, fp);
		break;

#ifdef macintosh
	case PY_RESOURCE:
		m = PyMac_LoadResourceModule(name, buf);
		break;
#endif

	case PKG_DIRECTORY:
		m = load_package(name, buf);
		break;

	case C_BUILTIN:
	case PY_FROZEN:
		if (type == C_BUILTIN)
			err = init_builtin(name);
		else
			err = PyImport_ImportFrozenModule(name);
		if (err < 0)
			goto failure;
		if (err == 0) {
			PyErr_Format(PyExc_ImportError,
				     "Purported %s module %.200s not found",
				     type == C_BUILTIN ?
						"builtin" : "frozen",
				     name);
			goto failure;
		}
		modules = PyImport_GetModuleDict();
		m = PyDict_GetItemString(modules, name);
		if (m == NULL) {
			PyErr_Format(
				PyExc_ImportError,
				"%s module %.200s not properly initialized",
				type == C_BUILTIN ?
					"builtin" : "frozen",
				name);
			goto failure;
		}
		Py_INCREF(m);
		break;

	default:
	  failure:
		PyErr_Format(PyExc_ImportError,
			     "Don't know how to import %.200s (type code %d)",
			      name, type);
		m = NULL;

	}

	return m;
}


/* Initialize a built-in module.
   Return 1 for succes, 0 if the module is not found, and -1 with
   an exception set if the initialization failed. */

static int
init_builtin(name)
	char *name;
{
	struct _inittab *p;
	PyObject *mod;

	if ((mod = _PyImport_FindExtension(name, name)) != NULL)
		return 1;

	for (p = _PyImport_Inittab; p->name != NULL; p++) {
		if (strcmp(name, p->name) == 0) {
			if (p->initfunc == NULL) {
				PyErr_Format(PyExc_ImportError,
				    "Cannot re-init internal module %.200s",
				    name);
				return -1;
			}
			if (Py_VerboseFlag)
				fprintf(stderr, "import %s # builtin\n", name);
			(*p->initfunc)();
			if (PyErr_Occurred())
				return -1;
			if (_PyImport_FixupExtension(name, name) == NULL)
				return -1;
			return 1;
		}
	}
	return 0;
}


/* Frozen modules */

static struct _frozen *
find_frozen(name)
	char *name;
{
	struct _frozen *p;

	for (p = PyImport_FrozenModules; ; p++) {
		if (p->name == NULL)
			return NULL;
		if (strcmp(p->name, name) == 0)
			break;
	}
	return p;
}

static PyObject *
get_frozen_object(name)
	char *name;
{
	struct _frozen *p = find_frozen(name);

	if (p == NULL) {
		PyErr_Format(PyExc_ImportError,
			     "No such frozen object named %.200s",
			     name);
		return NULL;
	}
	return PyMarshal_ReadObjectFromString((char *)p->code, p->size);
}

/* Initialize a frozen module.
   Return 1 for succes, 0 if the module is not found, and -1 with
   an exception set if the initialization failed.
   This function is also used from frozenmain.c */

int
PyImport_ImportFrozenModule(name)
	char *name;
{
	struct _frozen *p = find_frozen(name);
	PyObject *co;
	PyObject *m;

	if (p == NULL)
		return 0;
	if (Py_VerboseFlag)
		fprintf(stderr, "import %s # frozen\n", name);
	co = PyMarshal_ReadObjectFromString((char *)p->code, p->size);
	if (co == NULL)
		return -1;
	if (!PyCode_Check(co)) {
		Py_DECREF(co);
		PyErr_Format(PyExc_TypeError,
			     "frozen object %.200s is not a code object",
			     name);
		return -1;
	}
	m = PyImport_ExecCodeModule(name, co);
	Py_DECREF(co);
	if (m == NULL)
		return -1;
	Py_DECREF(m);
	return 1;
}


/* Import a module, either built-in, frozen, or external, and return
   its module object WITH INCREMENTED REFERENCE COUNT */

PyObject *
PyImport_ImportModule(name)
	char *name;
{
	return PyImport_ImportModuleEx(name, NULL, NULL, NULL);
}

PyObject *
PyImport_ImportModuleEx(name, globals, locals, fromlist)
	char *name;
	PyObject *globals;
	PyObject *locals;
	PyObject *fromlist;
{
	PyObject *modules = PyImport_GetModuleDict();
	PyObject *m;

	if ((m = PyDict_GetItemString(modules, name)) != NULL) {
		Py_INCREF(m);
	}
	else {
		char buf[MAXPATHLEN+1];
		struct filedescr *fdp;
		FILE *fp = NULL;

		buf[0] = '\0';
		fdp = find_module(name, (PyObject *)NULL,
				  buf, MAXPATHLEN+1, &fp);
		if (fdp == NULL)
			return NULL;
		m = load_module(name, fp, buf, fdp->type);
		if (fp)
			fclose(fp);
	}

	return m;
}


/* Re-import a module of any kind and return its module object, WITH
   INCREMENTED REFERENCE COUNT */

PyObject *
PyImport_ReloadModule(m)
	PyObject *m;
{
	PyObject *modules = PyImport_GetModuleDict();
	char *name;
	char buf[MAXPATHLEN+1];
	struct filedescr *fdp;
	FILE *fp = NULL;

	if (m == NULL || !PyModule_Check(m)) {
		PyErr_SetString(PyExc_TypeError,
				"reload() argument must be module");
		return NULL;
	}
	name = PyModule_GetName(m);
	if (name == NULL)
		return NULL;
	if (m != PyDict_GetItemString(modules, name)) {
		PyErr_Format(PyExc_ImportError,
			     "reload(): module %.200s not in sys.modules",
			     name);
		return NULL;
	}
	buf[0] = '\0';
	fdp = find_module(name, (PyObject *)NULL, buf, MAXPATHLEN+1, &fp);
	if (fdp == NULL)
		return NULL;
	m = load_module(name, fp, buf, fdp->type);
	if (fp)
		fclose(fp);
	return m;
}


/* Higher-level import emulator which emulates the "import" statement
   more accurately -- it invokes the __import__() function from the
   builtins of the current globals.  This means that the import is
   done using whatever import hooks are installed in the current
   environment, e.g. by "ni" or "rexec". */

PyObject *
PyImport_Import(module_name)
	PyObject *module_name;
{
	static PyObject *silly_list = NULL;
	static PyObject *builtins_str = NULL;
	static PyObject *import_str = NULL;
	static PyObject *standard_builtins = NULL;
	PyObject *globals = NULL;
	PyObject *import = NULL;
	PyObject *builtins = NULL;
	PyObject *r = NULL;

	/* Initialize constant string objects */
	if (silly_list == NULL) {
		import_str = PyString_InternFromString("__import__");
		if (import_str == NULL)
			return NULL;
		builtins_str = PyString_InternFromString("__builtins__");
		if (builtins_str == NULL)
			return NULL;
		silly_list = Py_BuildValue("[s]", "__doc__");
		if (silly_list == NULL)
			return NULL;
	}

	/* Get the builtins from current globals */
	globals = PyEval_GetGlobals();
	if(globals != NULL) {
		builtins = PyObject_GetItem(globals, builtins_str);
		if (builtins == NULL)
			goto err;
	}
	else {
		/* No globals -- use standard builtins, and fake globals */
		PyErr_Clear();

		if (standard_builtins == NULL) {
			standard_builtins =
				PyImport_ImportModule("__builtin__");
			if (standard_builtins == NULL)
				return NULL;
		}

		builtins = standard_builtins;
		Py_INCREF(builtins);
		globals = Py_BuildValue("{OO}", builtins_str, builtins);
		if (globals == NULL)
			goto err;
	}

	/* Get the __import__ function from the builtins */
	if (PyDict_Check(builtins))
		import=PyObject_GetItem(builtins, import_str);
	else
		import=PyObject_GetAttr(builtins, import_str);
	if (import == NULL)
		goto err;

	/* Call the _import__ function with the proper argument list */
	r = PyObject_CallFunction(import, "OOOO",
				  module_name, globals, globals, silly_list);

  err:
	Py_XDECREF(globals);
	Py_XDECREF(builtins);
	Py_XDECREF(import);
 
	return r;
}


/* Module 'imp' provides Python access to the primitives used for
   importing modules.
*/

static PyObject *
imp_get_magic(self, args)
	PyObject *self;
	PyObject *args;
{
	char buf[4];

	if (!PyArg_ParseTuple(args, ""))
		return NULL;
	buf[0] = (char) ((MAGIC >>  0) & 0xff);
	buf[1] = (char) ((MAGIC >>  8) & 0xff);
	buf[2] = (char) ((MAGIC >> 16) & 0xff);
	buf[3] = (char) ((MAGIC >> 24) & 0xff);

	return PyString_FromStringAndSize(buf, 4);
}

static PyObject *
imp_get_suffixes(self, args)
	PyObject *self;
	PyObject *args;
{
	PyObject *list;
	struct filedescr *fdp;

	if (!PyArg_ParseTuple(args, ""))
		return NULL;
	list = PyList_New(0);
	if (list == NULL)
		return NULL;
	for (fdp = _PyImport_Filetab; fdp->suffix != NULL; fdp++) {
		PyObject *item = Py_BuildValue("ssi",
				       fdp->suffix, fdp->mode, fdp->type);
		if (item == NULL) {
			Py_DECREF(list);
			return NULL;
		}
		if (PyList_Append(list, item) < 0) {
			Py_DECREF(list);
			Py_DECREF(item);
			return NULL;
		}
		Py_DECREF(item);
	}
	return list;
}

static PyObject *
call_find_module(name, path)
	char *name;
	PyObject *path; /* list or NULL */
{
	extern int fclose Py_PROTO((FILE *));
	PyObject *fob, *ret;
	struct filedescr *fdp;
	char pathname[MAXPATHLEN+1];
	FILE *fp = NULL;

	pathname[0] = '\0';
	fdp = find_module(name, path, pathname, MAXPATHLEN+1, &fp);
	if (fdp == NULL)
		return NULL;
	if (fp != NULL) {
		fob = PyFile_FromFile(fp, pathname, fdp->mode, fclose);
		if (fob == NULL) {
			fclose(fp);
			return NULL;
		}
	}
	else {
		fob = Py_None;
		Py_INCREF(fob);
	}		
	ret = Py_BuildValue("Os(ssi)",
		      fob, pathname, fdp->suffix, fdp->mode, fdp->type);
	Py_DECREF(fob);
	return ret;
}

static PyObject *
imp_find_module(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	PyObject *path = NULL;
	if (!PyArg_ParseTuple(args, "s|O!", &name, &PyList_Type, &path))
		return NULL;
	return call_find_module(name, path);
}

static PyObject *
imp_init_builtin(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	int ret;
	PyObject *m;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	ret = init_builtin(name);
	if (ret < 0)
		return NULL;
	if (ret == 0) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	m = PyImport_AddModule(name);
	Py_XINCREF(m);
	return m;
}

static PyObject *
imp_init_frozen(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	int ret;
	PyObject *m;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	ret = PyImport_ImportFrozenModule(name);
	if (ret < 0)
		return NULL;
	if (ret == 0) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	m = PyImport_AddModule(name);
	Py_XINCREF(m);
	return m;
}

static PyObject *
imp_get_frozen_object(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	return get_frozen_object(name);
}

static PyObject *
imp_is_builtin(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	return PyInt_FromLong(is_builtin(name));
}

static PyObject *
imp_is_frozen(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	return PyInt_FromLong(is_frozen(name));
}

static FILE *
get_file(pathname, fob, mode)
	char *pathname;
	PyObject *fob;
	char *mode;
{
	FILE *fp;
	if (fob == NULL) {
		fp = fopen(pathname, mode);
		if (fp == NULL)
			PyErr_SetFromErrno(PyExc_IOError);
	}
	else {
		fp = PyFile_AsFile(fob);
		if (fp == NULL)
			PyErr_SetString(PyExc_ValueError,
					"bad/closed file object");
	}
	return fp;
}

static PyObject *
imp_load_compiled(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	char *pathname;
	PyObject *fob = NULL;
	PyObject *m;
	FILE *fp;
	if (!PyArg_ParseTuple(args, "ss|O!", &name, &pathname,
			      &PyFile_Type, &fob))
		return NULL;
	fp = get_file(pathname, fob, "rb");
	if (fp == NULL)
		return NULL;
	m = load_compiled_module(name, pathname, fp);
	if (fob == NULL)
		fclose(fp);
	return m;
}

static PyObject *
imp_load_dynamic(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	char *pathname;
	PyObject *fob = NULL;
	PyObject *m;
	FILE *fp = NULL;
	if (!PyArg_ParseTuple(args, "ss|O!", &name, &pathname,
			      &PyFile_Type, &fob))
		return NULL;
	if (fob) {
		fp = get_file(pathname, fob, "r");
		if (fp == NULL)
			return NULL;
	}
	m = _PyImport_LoadDynamicModule(name, pathname, fp);
	return m;
}

static PyObject *
imp_load_source(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	char *pathname;
	PyObject *fob = NULL;
	PyObject *m;
	FILE *fp;
	if (!PyArg_ParseTuple(args, "ss|O!", &name, &pathname,
			      &PyFile_Type, &fob))
		return NULL;
	fp = get_file(pathname, fob, "r");
	if (fp == NULL)
		return NULL;
	m = load_source_module(name, pathname, fp);
	if (fob == NULL)
		fclose(fp);
	return m;
}

#ifdef macintosh
static PyObject *
imp_load_resource(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	char *pathname;
	PyObject *m;

	if (!PyArg_ParseTuple(args, "ss", &name, &pathname))
		return NULL;
	m = PyMac_LoadResourceModule(name, pathname);
	return m;
}
#endif /* macintosh */

static PyObject *
imp_load_module(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	PyObject *fob;
	char *pathname;
	char *suffix; /* Unused */
	char *mode;
	int type;
	FILE *fp;

	if (!PyArg_ParseTuple(args, "sOs(ssi)",
			      &name, &fob, &pathname,
			      &suffix, &mode, &type))
		return NULL;
	if (*mode && (*mode != 'r' || strchr(mode, '+') != NULL)) {
		PyErr_Format(PyExc_ValueError,
			     "invalid file open mode %.200s", mode);
		return NULL;
	}
	if (fob == Py_None)
		fp = NULL;
	else {
		if (!PyFile_Check(fob)) {
			PyErr_SetString(PyExc_ValueError,
				"load_module arg#2 should be a file or None");
			return NULL;
		}
		fp = get_file(pathname, fob, mode);
		if (fp == NULL)
			return NULL;
	}
	return load_module(name, fp, pathname, type);
}

static PyObject *
imp_load_package(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	char *pathname;
	if (!PyArg_ParseTuple(args, "ss", &name, &pathname))
		return NULL;
	return load_package(name, pathname);
}

static PyObject *
imp_new_module(self, args)
	PyObject *self;
	PyObject *args;
{
	char *name;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	return PyModule_New(name);
}

static PyObject *
imp_find_module_in_package(self, args)
	PyObject *self;
	PyObject *args;
{
	PyObject *name;
	PyObject *packagename = NULL;
	PyObject *package;
	PyObject *modules;
	PyObject *path;

	if (!PyArg_ParseTuple(args, "S|S", &name, &packagename))
		return NULL;
	if (packagename == NULL || PyString_GET_SIZE(packagename) == 0) {
		return call_find_module(
			PyString_AS_STRING(name),
			(PyObject *)NULL);
	}
	modules = PyImport_GetModuleDict();
	package = PyDict_GetItem(modules, packagename);
	if (package == NULL) {
		PyErr_Format(PyExc_ImportError,
			     "No package named %.200s",
			     PyString_AS_STRING(packagename));
		return NULL;
	}
	path = PyObject_GetAttrString(package, "__path__");
	if (path == NULL) {
		PyErr_Format(PyExc_ImportError,
			     "Package %.200s has no __path__ attribute",
			     PyString_AS_STRING(packagename));
		return NULL;
	}
	return call_find_module(PyString_AS_STRING(name), path);
}

static PyObject *
imp_find_module_in_directory(self, args)
	PyObject *self;
	PyObject *args;
{
	PyObject *name;
	PyObject *directory;
	PyObject *path;

	if (!PyArg_ParseTuple(args, "SS", &name, &directory))
		return NULL;
	path = Py_BuildValue("[O]", directory);
	if (path == NULL)
		return NULL;
	return call_find_module(PyString_AS_STRING(name), path);
}

static PyMethodDef imp_methods[] = {
	{"find_module",		imp_find_module,	1},
	{"find_module_in_directory",	imp_find_module_in_directory,	1},
	{"find_module_in_package",	imp_find_module_in_package,	1},
	{"get_frozen_object",	imp_get_frozen_object,	1},
	{"get_magic",		imp_get_magic,		1},
	{"get_suffixes",	imp_get_suffixes,	1},
	{"init_builtin",	imp_init_builtin,	1},
	{"init_frozen",		imp_init_frozen,	1},
	{"is_builtin",		imp_is_builtin,		1},
	{"is_frozen",		imp_is_frozen,		1},
	{"load_compiled",	imp_load_compiled,	1},
	{"load_dynamic",	imp_load_dynamic,	1},
	{"load_module",		imp_load_module,	1},
	{"load_package",	imp_load_package,	1},
#ifdef macintosh
	{"load_resource",	imp_load_resource,	1},
#endif
	{"load_source",		imp_load_source,	1},
	{"new_module",		imp_new_module,		1},
	{NULL,			NULL}		/* sentinel */
};

int
setint(d, name, value)
	PyObject *d;
	char *name;
	int value;
{
	PyObject *v;
	int err;

	v = PyInt_FromLong((long)value);
	err = PyDict_SetItemString(d, name, v);
	Py_XDECREF(v);
	return err;
}

void
initimp()
{
	PyObject *m, *d;

	m = Py_InitModule("imp", imp_methods);
	d = PyModule_GetDict(m);

	if (setint(d, "SEARCH_ERROR", SEARCH_ERROR) < 0) goto failure;
	if (setint(d, "PY_SOURCE", PY_SOURCE) < 0) goto failure;
	if (setint(d, "PY_COMPILED", PY_COMPILED) < 0) goto failure;
	if (setint(d, "C_EXTENSION", C_EXTENSION) < 0) goto failure;
	if (setint(d, "PY_RESOURCE", PY_RESOURCE) < 0) goto failure;
	if (setint(d, "PKG_DIRECTORY", PKG_DIRECTORY) < 0) goto failure;
	if (setint(d, "C_BUILTIN", C_BUILTIN) < 0) goto failure;
	if (setint(d, "PY_FROZEN", PY_FROZEN) < 0) goto failure;

  failure:
	;
}
