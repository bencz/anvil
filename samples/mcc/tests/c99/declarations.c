/*
 * C99 Test: Declaration Features
 * Tests C99 declaration features
 */

int main(void)
{
    /* C99: Mixed declarations and statements */
    int x = 10;
    x = x + 5;
    int y = 20;  /* Declaration after statement */
    y = y + x;
    
    /* C99: Declarations in for loop */
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += i;
    }
    
    /* Nested for with declarations */
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            sum += i * j;
        }
    }
    
    /* C99: Variable Length Arrays (VLA) */
    int n = 10;
    int vla[n];
    for (int i = 0; i < n; i++) {
        vla[i] = i * 2;
    }
    
    /* 2D VLA */
    int rows = 3, cols = 4;
    int matrix[rows][cols];
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            matrix[i][j] = i + j;
        }
    }
    
    /* VLA in function parameter would be tested separately */
    
    return sum;
}

/* Function with VLA parameter */
int sum_array(int n, int arr[n])
{
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += arr[i];
    }
    return total;
}

/* Function with 2D VLA parameter */
int sum_matrix(int rows, int cols, int mat[rows][cols])
{
    int total = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            total += mat[i][j];
        }
    }
    return total;
}
