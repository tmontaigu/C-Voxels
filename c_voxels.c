#include <Python.h>
#include <numpy/arrayobject.h>

#include "c_voxels.h"
#include "conversion_utilities.h"
#include "uthash.h"

//#include "pyconfig.h"



//=====================================================================
// Functions callable from Python
//=====================================================================
static PyObject* voxelize_cloud(PyObject *self, PyObject *args) {
    PyArrayObject *py_coords; // The numpy matrix of coordinates
    PyObject *py_classification, *py_class_black_list, *py_coords_min;
	

    double **c_coords, *c_coords_min; // The C matrix of coordinates
    int *c_classification, num_black_listed, num_points;
    double k;

    // Parse args from python call
	if (!PyArg_ParseTuple(args, "O!OOOd", &PyArray_Type, &py_coords, &py_classification, &py_class_black_list,
                          &py_coords_min, &k))
        return NULL;

    // Check that we actually get lists for some of the args
    if (!PySequence_Check(py_classification) || !PySequence_Check(py_class_black_list) ||
        !PySequence_Check(py_coords_min)) {
        PyErr_SetString(PyExc_TypeError, "expected sequence");
        return NULL;
    }

	num_points = py_coords->dimensions[0];
    num_black_listed = PyObject_Length(py_class_black_list);

	// Convert classification to a contiguous array that can be used in C
	PyArrayObject *classification_array;
	classification_array = (PyArrayObject *) PyArray_ContiguousFromObject(py_classification, PyArray_INT, 0, num_points);

    
	if (classification_array->nd != 1) {
		PySys_WriteStdout("Classification param is not a 1D list/Array\n");
	}
	if (classification_array->dimensions[0] != num_points) {
		PySys_WriteStdout("Error: Classification is not the same length as the number of points");
	}
	
	int *c_class_black_list = NULL;
	if (num_black_listed > 0) {
		c_class_black_list = py_int_list_to_c_array(py_class_black_list);
	}

	c_classification = classification_array->data;
    c_coords_min = py_double_list_to_c_array(py_coords_min);
    c_coords = py_matrix_to_c_array(py_coords);

	
    struct Voxel *current_voxel, *tmp, *voxels = NULL;
    voxels = compute_voxels(c_coords, c_classification, c_class_black_list, c_coords_min, k, num_points, num_black_listed);

    int voxel_count = 0;
    int point_count = 0, point_count2 = 0;
    struct Point *current_point, *tmp_point;
    PyObject *voxels_dict = PyDict_New();

    HASH_ITER(hh, voxels, current_voxel, tmp) {
        int index = 0;
        PyObject *list_of_points = PyList_New(current_voxel->num_points);
        PyObject *key = Py_BuildValue("(i,i,i)", current_voxel->coord.x, current_voxel->coord.y, current_voxel->coord.z);

        LL_FOREACH_SAFE(current_voxel->points, current_point, tmp_point) {
            PyList_SetItem(list_of_points, index, Py_BuildValue("i", current_point->index));
            LL_DELETE(current_voxel->points, current_point);
            free(current_point);
            ++point_count;
            ++index;
        }
        PyDict_SetItem(voxels_dict, key, list_of_points);
        point_count2 += current_voxel->num_points;


        HASH_DEL(voxels, current_voxel);
        free(current_voxel);
        ++voxel_count;
    }


    free_c_array(c_coords);
    free(c_coords_min);
	free(c_class_black_list);

    return voxels_dict;
}



static PyObject *neighbours_of_voxels(PyObject *self, PyObject *args) {
	PyObject *py_voxels, *py_keys;
    struct Voxel *c_voxels = NULL, *v;

	// Parse args from python call
	if (!PyArg_ParseTuple(args, "O", &py_keys))
        return NULL;

	// Check that we got the good argument
	if (!PyList_Check(py_keys)) {
		PyErr_SetString(PyExc_TypeError, "Expected a List");
	}

	int num_voxels = PyList_Size(py_keys);

    if(!PySequence_Check(py_keys)) {
        PyErr_SetString(PyExc_TypeError, "Keys are not a sequence");
    }

    // Creating the PyObject that we will return
    PyObject *neighbours = PyDict_New();


    // Buil the hash table of voxels
    // Caring only about the keys, not the values
	for (int i = 0; i < num_voxels; ++i) {
		PyObject *key = PyList_GetItem(py_keys, i);
		struct Coordinates c = new_coordinates_from_py_tuple(key);
		v = new_voxel(c, i);
        HASH_ADD(hh, c_voxels, coord, sizeof(struct Coordinates), v);
	}

    // Now loop over the voxels to find their neighbours
    struct Voxel *voxel, *p;
    PyObject *current_list;
    for(voxel = c_voxels; voxel != NULL; voxel = voxel->hh.next) {

        struct Coordinates top = voxel->coord, bot = voxel->coord, left = voxel->coord,
                           right = voxel->coord, front = voxel->coord, back = voxel->coord;
        top.z += 1; bot.z -= 1; right.x += 1; left.x -= 1; front.y += 1; back.y -= 1;
        struct Coordinates potential_neighbours[6] = {top, bot, left, right, front, back};


        PyObject *neighbours_list = PyList_New(0);
        for (int k = 0; k < 6; ++k) {
            PyObject *neighbour_key = coordinates_to_py_tuple(potential_neighbours[k]);
            HASH_FIND(hh, c_voxels, &(potential_neighbours[k]), sizeof(struct Coordinates), p);

            if (p) {
                PyList_Append(neighbours_list, coordinates_to_py_tuple(potential_neighbours[k]));
            }
        }
        PyDict_SetItem(neighbours, coordinates_to_py_tuple(voxel->coord), neighbours_list);
    }


    HASH_ITER(hh, c_voxels, p, v) {
        HASH_DEL(c_voxels, p);
        free(p);
    }


    return neighbours;
}

static PyObject* version(PyObject* self)
{
    return Py_BuildValue("s", "Version 0.1");
}

//=====================================================================
// C functions
//=====================================================================

int is_black_listed(int code, int *black_list, int num_black_list) {
    for (int i = 0; i < num_black_list; ++i) {
        if (code == black_list[i]) {
            return 1;
        }
    }
    return 0;
}

static struct Coordinates get_voxel_coordinates(double x, double y, double z, double k, double *coords_min) {
    struct Coordinates voxel_coords;
    voxel_coords.x = (int)(((x - coords_min[0])) / k);
    voxel_coords.y = (int)(((y - coords_min[1])) / k);
    voxel_coords.z = (int)(((z - coords_min[2])) / k);
    return voxel_coords;
}

// TODO: Lets create a struct PointCloud !
static struct Voxel *compute_voxels(double **coords, int *classification, int *black_list, double *coords_min,
									double k, int num_points, int num_black_list) {

    struct Voxel *p = NULL, *voxels = NULL;

    for (int i = 0; i < num_points; ++i) {
        // TODO: change blacklist array to 0(1) acces to check is blacklisted
        // -> hash table or a precreated array of 0|1

        if (is_black_listed(classification[i], black_list, num_black_list)) {
            continue;
        }
        struct Coordinates c = get_voxel_coordinates(coords[i][0], coords[i][1], coords[i][2], k, coords_min);
        struct Voxel *v = new_voxel(c, i);

		if (!v) {
			return PyErr_NoMemory();
		}

        HASH_FIND(hh, voxels, &(v->coord), sizeof(struct Coordinates), p);

		struct Point *pp = new_point(i);

		if (!pp) {
			return PyErr_NoMemory();
		}

        if (!p) { // Voxel not found in hash, add it
            HASH_ADD(hh, voxels, coord, sizeof(struct Coordinates), v);
            LL_PREPEND(v->points, pp);
            v->num_points += 1;
        }
        else {
            free(v); //Voxel already exist, delete the one we just created
            LL_PREPEND(p->points, pp);
            p->num_points += 1;

        }
    }
    return voxels;
}

//=====================================================================
// Other things
//=====================================================================
struct Voxel *new_voxel(struct Coordinates coords, int index) {
	struct Voxel *v = (struct Voxel*) malloc(sizeof(struct Voxel));
	
	if (v) {
		memset(v, 0, sizeof(struct Voxel));
		v->coord = coords;
		v->points = NULL;
		v->num_points = 0;
		v->index = index;
	}
	return v;
}

struct Point *new_point(int index) {
	struct Point *point = (struct Point*) malloc(sizeof(struct Point));

	if (point) {
		point->next = NULL;
		point->index = index;
	}
	return point;
}

struct Coordinates new_coordinates_from_py_tuple(PyObject *tuple) {
	struct Coordinates c = {
		PyInt_AsLong(PyTuple_GetItem(tuple, 0)),
		PyInt_AsLong(PyTuple_GetItem(tuple, 1)),
		PyInt_AsLong(PyTuple_GetItem(tuple, 2))
	};
	return c;
}

PyObject *coordinates_to_py_tuple(struct Coordinates c) {
	return Py_BuildValue("(i,i,i)", c.x, c.y, c.z);
}


//=====================================================================
// Python API function to register module
//=====================================================================

static PyMethodDef cvoxel_methods[] = {
    {"voxelize_cloud", voxelize_cloud, METH_VARARGS, "Voxelize the cloud"},
    {"version", (PyCFunction)version, METH_NOARGS, "Returns de module version"},
    {"neighbours_of_voxels", neighbours_of_voxels, METH_VARARGS, "ayy lmao"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC initcvoxels(void) {
    (void) Py_InitModule("cvoxels", cvoxel_methods);
    import_array();
}