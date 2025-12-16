/*
 * C23 Test: Literal Features
 * Tests C23 literal enhancements
 */

int main(void)
{
    /* C23: Binary literals */
    int bin1 = 0b1010;          /* 10 */
    int bin2 = 0B11110000;      /* 240 */
    unsigned bin3 = 0b11111111; /* 255 */
    
    /* C23: Digit separators */
    int million = 1'000'000;
    long long big = 1'234'567'890'123LL;
    int hex = 0xFF'FF'FF'FF;
    int bin = 0b1111'0000'1111'0000;
    double pi = 3.141'592'653'589;
    
    /* C23: u8 character literals */
    unsigned char u8char = u8'A';
    
    return bin1 + bin2;
}
