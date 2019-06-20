#include<stdio.h>
#include<string.h>
#include <stdlib.h>

void strToBin(char *str, char *bin, int len)
{
    int i = 0;
    int j = 0;
    char token[3] = {0};
    
    for (i = 0; i < strlen(str) && j < len; )
    {
        memset(token, 0 ,sizeof(token));
        memcpy(token, str + i, 2);

        int num = strtoul(token, NULL, 16);
        printf("%s ", token);
        i = i + 2;

        bin[j] = num;
        j++;
    }

    printf("\n");
    for (i = 0; i < j; i++)
    {
        printf("%u ", (unsigned char)bin[i]);
    }
    printf("\n");
}


int main()
{
    char str[] = "8daef956a98d188ee55a624928546016";
    char bin[256] = {0};
    strToBin(str, bin, 16);

    char sha[] = "42767560b06f66fc0fd6acbb975a684adb68c190";
    char sha_bin[256] = {0};
    strToBin(sha, sha_bin, 32);
    
    return 0;
}
