// 13 - sorting, the program the plan uses as the exit criterion for composite memory.
//
// One program that exercises indexed loads, indexed stores, comparison and a swap all at once,
// which is why it is the test that matters: any one of those getting the addressing wrong shows
// up as a wrong digit rather than a crash.
//
// Three sorts over the same data, each one a different shape of loop, plus a binary search over
// the result.
//
//     ceresc examples/13_sorting.c --run

void put(char c)
{
    char* terminal = (char*)0xFF000004;
    *terminal = c;
}

void putstr(char* text)
{
    for (int i = 0; text[i] != 0; i++)
        put(text[i]);
}

void putint(int value)
{
    if (value < 0)
    {
        put('-');
        value = -value;
    }
    if (value >= 10)
        putint(value / 10);
    put('0' + value % 10);
}

void printArray(char* label, int* values, int count)
{
    putstr(label);
    for (int i = 0; i < count; i++)
    {
        putint(values[i]);
        put(' ');
    }
    put('\n');
}

void copyInto(int* destination, int* source, int count)
{
    for (int i = 0; i < count; i++)
        destination[i] = source[i];
}

void selectionSort(int* values, int count)
{
    for (int i = 0; i < count - 1; i++)
    {
        int smallest = i;
        for (int j = i + 1; j < count; j++)
        {
            if (values[j] < values[smallest])
                smallest = j;
        }
        int temp = values[i];
        values[i] = values[smallest];
        values[smallest] = temp;
    }
}

void bubbleSort(int* values, int count)
{
    bool swapped = true;
    int limit = count - 1;
    while (swapped)
    {
        swapped = false;
        for (int i = 0; i < limit; i++)
        {
            if (values[i] > values[i + 1])
            {
                int temp = values[i];
                values[i] = values[i + 1];
                values[i + 1] = temp;
                swapped = true;
            }
        }
        limit--;
    }
}

void insertionSort(int* values, int count)
{
    for (int i = 1; i < count; i++)
    {
        int key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key)
        {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

// Returns the index of `target`, or -1. Only correct on a sorted array, which is what makes it a
// check on the sorts above as well as on itself.
int binarySearch(int* values, int count, int target)
{
    int low = 0;
    int high = count - 1;
    while (low <= high)
    {
        int middle = low + (high - low) / 2;
        if (values[middle] == target)
            return middle;
        if (values[middle] < target)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return -1;
}

int main(void)
{
    int original[10] = { 5, 3, 9, 1, 7, 0, 8, 2, 6, 4 };
    int working[10];

    printArray("unsorted:   ", original, 10);

    copyInto(working, original, 10);
    selectionSort(working, 10);
    printArray("selection:  ", working, 10);

    copyInto(working, original, 10);
    bubbleSort(working, 10);
    printArray("bubble:     ", working, 10);

    copyInto(working, original, 10);
    insertionSort(working, 10);
    printArray("insertion:  ", working, 10);

    putstr("search:     ");
    for (int target = 0; target < 12; target++)
    {
        putint(binarySearch(working, 10, target));
        put(' ');
    }
    putstr("(-1 means not found)\n");

    // Already sorted, and sorted backwards: the two cases that break a loop bound off by one.
    int ascending[5] = { 1, 2, 3, 4, 5 };
    int descending[5] = { 5, 4, 3, 2, 1 };
    insertionSort(ascending, 5);
    insertionSort(descending, 5);
    printArray("edge cases: ", ascending, 5);
    printArray("            ", descending, 5);

    return 0;
}
