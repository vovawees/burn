/* bt-split: the library (-DLIB) and the program that links against it */
#ifdef LIB
int btsplit(void)
{
	return 42;
}
#else
int btsplit(void);

int main(void)
{
	return btsplit() == 42 ? 0 : 1;
}
#endif
