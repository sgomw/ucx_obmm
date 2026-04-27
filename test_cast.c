#include <stdio.h>
#include <stddef.h>

int main() {
    unsigned a = 65535;
    unsigned b = 65535;
    
    // This should cast a to size_t first, then multiply
    size_t result1 = ((size_t)a * b);
    
    // This would overflow if done in unsigned space first
    unsigned temp = a * b;
    size_t result2 = temp;
    
    printf("Method 1 (cast first): %zu\n", result1);
    printf("Method 2 (mult first): %zu\n", result2);
    printf("Expected: %zu\n", (size_t)65535 * 65535);
    
    return 0;
}
