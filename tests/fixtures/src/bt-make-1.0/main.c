#include <stdio.h>
#include "btm.h"

int main(void)
{
	printf("%s, %s\n", GREETING, btm_name());
	return 0;
}
