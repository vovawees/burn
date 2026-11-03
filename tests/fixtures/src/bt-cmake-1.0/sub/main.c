#include <stdio.h>

int main(void)
{
#ifdef BT_FOO
	puts("foo " BT_BAR);
#else
	puts("no foo " BT_BAR);
#endif
	return 0;
}
