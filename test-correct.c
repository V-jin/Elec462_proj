#include <stdio.h>
int main() {
    int n, num, sum = 0;
    if (scanf("%d", &n) != 1) return 0;
    for (int i = 0; i < n; i++) {
        if (scanf("%d", &num) == 1) sum += num;
    }
    printf("%d\n", sum); //올바른 코드
    return 0;
}
