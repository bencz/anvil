/*
 * C89 Test: Control Flow
 * Tests all C89 control flow statements
 */

int main(void)
{
    int i, j, sum = 0;
    int arr[5] = {1, 2, 3, 4, 5};
    
    /* if statement */
    if (sum == 0) {
        sum = 1;
    }
    
    /* if-else statement */
    if (sum > 0) {
        sum = sum + 1;
    } else {
        sum = 0;
    }
    
    /* if-else if-else chain */
    if (sum < 0) {
        sum = -sum;
    } else if (sum == 0) {
        sum = 1;
    } else {
        sum = sum * 2;
    }
    
    /* while loop */
    i = 0;
    while (i < 5) {
        sum = sum + arr[i];
        i = i + 1;
    }
    
    /* do-while loop */
    i = 0;
    do {
        sum = sum + 1;
        i = i + 1;
    } while (i < 3);
    
    /* for loop */
    for (i = 0; i < 5; i++) {
        sum = sum + i;
    }
    
    /* nested loops */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            sum = sum + i * j;
        }
    }
    
    /* break statement */
    for (i = 0; i < 10; i++) {
        if (i == 5) {
            break;
        }
        sum = sum + i;
    }
    
    /* continue statement */
    for (i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            continue;
        }
        sum = sum + i;
    }
    
    /* switch statement */
    switch (sum % 4) {
        case 0:
            sum = sum + 10;
            break;
        case 1:
            sum = sum + 20;
            break;
        case 2:
        case 3:
            sum = sum + 30;
            break;
        default:
            sum = 0;
            break;
    }
    
    /* goto statement */
    i = 0;
loop:
    if (i < 3) {
        sum = sum + i;
        i = i + 1;
        goto loop;
    }
    
    /* return statement */
    return sum;
}
