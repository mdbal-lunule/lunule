// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 John Spray <john.spray@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */


#include "BaseMgrStandbyModule.h"

#include "StandbyPyModules.h"


#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr

typedef struct {
  PyObject_HEAD
  StandbyPyModule *this_module;
} BaseMgrStandbyModule;

static PyObject *
BaseMgrStandbyModule_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    BaseMgrStandbyModule *self;

    self = (BaseMgrStandbyModule *)type->tp_alloc(type, 0);

    return (PyObject *)self;
}

static int
BaseMgrStandbyModule_init(BaseMgrStandbyModule *self, PyObject *args, PyObject *kwds)
{
    PyObject *this_module_capsule = nullptr;
    static const char *kwlist[] = {"this_module", NULL};

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                      const_cast<char**>(kwlist),
                                      &this_module_capsule)) {
        return -1;
    }

    self->this_module = (StandbyPyModule*)PyCapsule_GetPointer(
        this_module_capsule, nullptr);
    assert(self->this_module);

    return 0;
}

static PyObject*
ceph_get_mgr_id(BaseMgrStandbyModule *self, PyObject *args)
{
  return PyString_FromString(g_conf->name.get_id().c_str());
}

static PyObject*
ceph_config_get(BaseMgrStandbyModule *self, PyObject *args)
{
  char *what = nullptr;
  if (!PyArg_ParseTuple(args, "s:ceph_config_get", &what)) {
    derr << "Invalid args!" << dendl;
    return nullptr;
  }

  std::string value;
  bool found = self->this_module->get_config(what, &value);
  if (found) {
    dout(10) << "ceph_config_get " << what << " found: " << value.c_str() << dendl;
    return PyString_FromString(value.c_str());
  } else {
    dout(4) << "ceph_config_get " << what << " not found " << dendl;
    Py_RETURN_NONE;
  }
}

static PyObject*
ceph_get_active_uri(BaseMgrStandbyModule *self, PyObject *args)
{
  return PyString_FromString(self->this_module->get_active_uri().c_str());
}

static PyObject*
ceph_log(BaseMgrStandbyModule *self, PyObject *args)
{
  int level = 0;
  char *record = nullptr;
  if (!PyArg_ParseTuple(args, "is:log", &level, &record)) {
    return nullptr;
  }

  assert(self->this_module);

  self->this_module->log(level, record);

  Py_RETURN_NONE;
}

PyMethodDef BaseMgrStandbyModule_methods[] = {

  {"_ceph_get_mgr_id", (PyCFunction)ceph_get_mgr_id, METH_NOARGS,
   "Get the name of the Mgr daemon where we are running"},

  {"_ceph_get_config", (PyCFunction)ceph_config_get, METH_VARARGS,
   "Get a configuration value"},

  {"_ceph_get_active_uri", (PyCFunction)ceph_get_active_uri, METH_NOARGS,
   "Get the URI of the active instance of this module, if any"},

  {"_ceph_log", (PyCFunction)ceph_log, METH_VARARGS,
   "Emit a log message"},

  {NULL, NULL, 0, NULL}
};

PyTypeObject BaseMgrStandbyModuleType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "ceph_module.BaseMgrStandbyModule", /* tp_name */
  sizeof(BaseMgrStandbyModule),     /* tp_basicsize */
  0,                         /* tp_itemsize */
  0,                         /* tp_dealloc */
  0,                         /* tp_print */
  0,                         /* tp_getattr */
  0,                         /* tp_setattr */
  0,                         /* tp_compare */
  0,                         /* tp_repr */
  0,                         /* tp_as_number */
  0,                         /* tp_as_sequence */
  0,                         /* tp_as_mapping */
  0,                         /* tp_hash */
  0,                         /* tp_call */
  0,                         /* tp_str */
  0,                         /* tp_getattro */
  0,                         /* tp_setattro */
  0,                         /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,        /* tp_flags */
  "ceph-mgr Standby Python Plugin", /* tp_doc */
  0,                         /* tp_traverse */
  0,                         /* tp_clear */
  0,                         /* tp_richcompare */
  0,                         /* tp_weaklistoffset */
  0,                         /* tp_iter */
  0,                         /* tp_iternext */
  BaseMgrStandbyModule_methods,     /* tp_methods */
  0,                         /* tp_members */
  0,                         /* tp_getset */
  0,                         /* tp_base */
  0,                         /* tp_dict */
  0,                         /* tp_descr_get */
  0,                         /* tp_descr_set */
  0,                         /* tp_dictoffset */
  (initproc)BaseMgrStandbyModule_init,                         /* tp_init */
  0,                         /* tp_alloc */
  BaseMgrStandbyModule_new,     /* tp_new */
};
