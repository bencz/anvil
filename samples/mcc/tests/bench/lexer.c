/* A lexer's byte classification: one switch of 74 cases in seven classes. */
#include <stdio.h>

static unsigned char text[1 << 20];

static unsigned classify(unsigned char c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        return 1;
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h': case 'i': case 'j': case 'k': case 'l': case 'm':
    case 'n': case 'o': case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
        return 2;
    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'L': case 'M':
    case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
        return 3;
    case ' ': case '\t': case '\n':
        return 4;
    case '+': case '-': case '*': case '/': case '=':
        return 5;
    case '(': case ')': case '{': case '}': case ';':
        return 6;
    default:
        return 7;
    }
}

int main(void)
{
    static const char sample[] = "int x1 = (Alpha + beta_7) * 42; { y = z / 3; }\n\tif (k != 0) Run(q);\n";
    for (unsigned i = 0; i < sizeof(text); i++)
        text[i] = (unsigned char)sample[(i * 7 + i / 13) % (sizeof(sample) - 1)];
    unsigned h = 0;
    for (int rep = 0; rep < 40; rep++) {
        unsigned x = 0;
        for (unsigned i = 0; i < sizeof(text); i++)
            x = x * 31 + classify(text[i]);
        h ^= x + (unsigned)rep;
    }
    printf("lexer %u\n", h);
    return 0;
}
