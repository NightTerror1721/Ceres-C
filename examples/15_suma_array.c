// 15 - the program the tutorial walks through, stage by stage.
//
// docs/04-Tutorial-C-to-CASM.md follows this exact file from tokens to IR to CASM, so it is kept
// as small as the point allows. `suma_array` is the prior audit's own example: a loop, an indexed
// load and a call boundary the array's address crosses.
//
// The original version of this program in the architecture plan ends with
// `return suma_array(datos, 4);` and says the process exits with code 100. That part could not
// survive contact with the real VM: `ceres run` exits 0 on a clean halt and 1 on a fault, never
// with a program-chosen value - there is no register-to-exit-code channel at all. So the total is
// printed instead, which is the only way a program here reports anything.
//
//     ceresc examples/15_suma_array.c --emit-ir
//     ceresc examples/15_suma_array.c -o 15.casm
//     ceresc examples/15_suma_array.c --run

int suma_array(int* arr, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = total + arr[i];
    }
    return total;
}

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
    *terminal = c;
}

void putint(int value)
{
    if (value >= 10)
        putint(value / 10);
    put('0' + value % 10);
}

int main(void)
{
    int datos[4];
    datos[0] = 10;
    datos[1] = 20;
    datos[2] = 30;
    datos[3] = 40;

    putint(suma_array(datos, 4));
    put('\n');
    return 0;
}
