/* Импорт xmlrpc тянет pyexpat, а разбирать XML рецепту не нужно. */
#include "Python.h"

static PyModuleDef model_def = {PyModuleDef_HEAD_INIT, "pyexpat.model", NULL, -1, NULL};
static PyModuleDef errors_def = {PyModuleDef_HEAD_INIT, "pyexpat.errors", NULL, -1, NULL};
static PyModuleDef module_def = {PyModuleDef_HEAD_INIT, "pyexpat", NULL, -1, NULL};

PyMODINIT_FUNC
PyInit_pyexpat(void)
{
    PyObject *module = PyModule_Create(&module_def);
    PyObject *model = PyModule_Create(&model_def);
    PyObject *errors = PyModule_Create(&errors_def);
    if (!module || !model || !errors) {
        Py_XDECREF(module);
        Py_XDECREF(model);
        Py_XDECREF(errors);
        return NULL;
    }
    if (PyModule_AddObjectRef(module, "model", model) < 0 ||
        PyModule_AddObjectRef(module, "errors", errors) < 0) {
        Py_DECREF(model);
        Py_DECREF(errors);
        Py_DECREF(module);
        return NULL;
    }
    Py_DECREF(model);
    Py_DECREF(errors);
    return module;
}
