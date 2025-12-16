/*
 * Test program for ANVIL-generated Base64 library
 * 
 * Links with the generated base64_lib.s to test base64 encoding
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Functions from generated library */
extern char* base64_encode(const char* input, int len, int* out_len);
extern int base64_encoded_len(int input_len);

/* Reference implementation for comparison */
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* reference_base64_encode(const char* input, int len, int* out_len) {
    int output_len = ((len + 2) / 3) * 4;
    char* output = malloc(output_len + 1);
    if (!output) return NULL;
    
    int i = 0, j = 0;
    while (i < len) {
        int start_i = i;
        unsigned char b1 = (unsigned char)input[i++];
        unsigned char b2 = (i < len) ? (unsigned char)input[i++] : 0;
        unsigned char b3 = (i < len) ? (unsigned char)input[i++] : 0;
        
        int bytes_in_group = i - start_i;  /* 1, 2, or 3 */
        
        output[j++] = base64_table[b1 >> 2];
        output[j++] = base64_table[((b1 & 0x03) << 4) | (b2 >> 4)];
        output[j++] = (bytes_in_group < 2) ? '=' : base64_table[((b2 & 0x0F) << 2) | (b3 >> 6)];
        output[j++] = (bytes_in_group < 3) ? '=' : base64_table[b3 & 0x3F];
    }
    
    output[j] = '\0';
    if (out_len) *out_len = output_len;
    return output;
}

static int tests_passed = 0;
static int tests_failed = 0;

static void test_encode(const char* input, const char* expected) {
    int len = strlen(input);
    int out_len = 0;
    
    char* result = base64_encode(input, len, &out_len);
    
    if (result == NULL) {
        printf("  [FAIL] base64_encode(\"%s\") returned NULL\n", input);
        tests_failed++;
        return;
    }
    
    if (strcmp(result, expected) == 0) {
        printf("  [PASS] base64_encode(\"%s\") = \"%s\"\n", input, result);
        tests_passed++;
    } else {
        printf("  [FAIL] base64_encode(\"%s\"): got \"%s\", expected \"%s\"\n", 
               input, result, expected);
        tests_failed++;
    }
    
    free(result);
}

static void test_encoded_len(int input_len, int expected) {
    int result = base64_encoded_len(input_len);
    
    if (result == expected) {
        printf("  [PASS] base64_encoded_len(%d) = %d\n", input_len, result);
        tests_passed++;
    } else {
        printf("  [FAIL] base64_encoded_len(%d): got %d, expected %d\n", 
               input_len, result, expected);
        tests_failed++;
    }
}

static void test_binary_data(const unsigned char* data, int len, const char* description) {
    int out_len = 0;
    char* result = base64_encode((const char*)data, len, &out_len);
    
    int ref_len = 0;
    char* expected = reference_base64_encode((const char*)data, len, &ref_len);
    
    if (result == NULL) {
        printf("  [FAIL] %s: returned NULL\n", description);
        tests_failed++;
        free(expected);
        return;
    }
    
    if (strcmp(result, expected) == 0) {
        printf("  [PASS] %s: \"%s\"\n", description, result);
        tests_passed++;
    } else {
        printf("  [FAIL] %s: got \"%s\", expected \"%s\"\n", description, result, expected);
        tests_failed++;
    }
    
    free(result);
    free(expected);
}

int main(void) {
    printf("=== ANVIL Base64 Library Test ===\n\n");
    
    printf("Testing base64_encoded_len:\n");
    test_encoded_len(0, 0);
    test_encoded_len(1, 4);
    test_encoded_len(2, 4);
    test_encoded_len(3, 4);
    test_encoded_len(4, 8);
    test_encoded_len(5, 8);
    test_encoded_len(6, 8);
    test_encoded_len(10, 16);
    test_encoded_len(100, 136);
    
    printf("\nTesting base64_encode with strings:\n");
    
    /* Standard test vectors */
    test_encode("", "");
    test_encode("f", "Zg==");
    test_encode("fo", "Zm8=");
    test_encode("foo", "Zm9v");
    test_encode("foob", "Zm9vYg==");
    test_encode("fooba", "Zm9vYmE=");
    test_encode("foobar", "Zm9vYmFy");
    
    /* More test cases */
    test_encode("Hello", "SGVsbG8=");
    test_encode("Hello, World!", "SGVsbG8sIFdvcmxkIQ==");
    test_encode("ANVIL", "QU5WSUw=");
    test_encode("Base64 encoding test", "QmFzZTY0IGVuY29kaW5nIHRlc3Q=");
    test_encode("The quick brown fox jumps over the lazy dog", 
                "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==");
    
    printf("\nTesting base64_encode with binary data:\n");
    
    /* Binary data tests */
    unsigned char bin1[] = {0x00};
    test_binary_data(bin1, 1, "single null byte");
    
    unsigned char bin2[] = {0xFF};
    test_binary_data(bin2, 1, "single 0xFF byte");
    
    unsigned char bin3[] = {0x00, 0x00, 0x00};
    test_binary_data(bin3, 3, "three null bytes");
    
    unsigned char bin4[] = {0xFF, 0xFF, 0xFF};
    test_binary_data(bin4, 3, "three 0xFF bytes");
    
    unsigned char bin5[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    test_binary_data(bin5, 5, "bytes 1-5");
    
    unsigned char bin6[] = {0xDE, 0xAD, 0xBE, 0xEF};
    test_binary_data(bin6, 4, "0xDEADBEEF");
    
    unsigned char bin7[] = {0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B};
    test_binary_data(bin7, 9, "mixed binary");
    
    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d\n", tests_passed + tests_failed);
    
    if (tests_failed == 0) {
        printf("\nAll tests passed! The ANVIL-generated base64 library works correctly.\n");
        return 0;
    } else {
        printf("\nSome tests failed.\n");
        return 1;
    }
}
