#include <ffi.h>
#include <stdio.h>

static int add(int a, int b) { return a + b; }
static double addd(double a, double b) { return a + b; }

static void closure_add(ffi_cif *cif, void *ret, void **args, void *ud) {
    (void)cif;
    (void)ud;
    *(int *)ret = *(int *)args[0] + *(int *)args[1];
}

int main(void) {
    ffi_cif cif;
    ffi_type *at[2];
    void *av[2];
    int a = 20, b = 22, r = 0;
    double x = 1.5, y = 2.25, z = 0;
    ffi_closure *cl;
    void *code;
    int (*fn)(int, int);

    at[0] = &ffi_type_sint;
    at[1] = &ffi_type_sint;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint, at) != FFI_OK) return 1;
    av[0] = &a;
    av[1] = &b;
    ffi_call(&cif, (void (*)(void))add, &r, av);
    if (r != 42) { printf("int %d\n", r); return 2; }

    at[0] = &ffi_type_double;
    at[1] = &ffi_type_double;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_double, at) != FFI_OK) return 3;
    av[0] = &x;
    av[1] = &y;
    ffi_call(&cif, (void (*)(void))addd, &z, av);
    if (z != 3.75) { printf("double\n"); return 4; }

    at[0] = &ffi_type_sint;
    at[1] = &ffi_type_sint;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint, at) != FFI_OK) return 5;
    cl = ffi_closure_alloc(sizeof *cl, &code);
    if (!cl) return 6;
    if (ffi_prep_closure_loc(cl, &cif, closure_add, 0, code) != FFI_OK) return 7;
    fn = code;
    if (fn(20, 22) != 42) { printf("closure\n"); return 8; }
    ffi_closure_free(cl);
    printf("ffi: ok\n");
    return 0;
}
