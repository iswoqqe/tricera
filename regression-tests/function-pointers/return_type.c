char hundred()
{
    return 100;
}

int thousand()
{
    return 1000;
}

/*@ contract @*/
int main()
{
    // ensure that preprocessor does not remove functions
    hundred();
    thousand();
    // test begins here
    char (*f)() = hundred;
    int (*g)() = thousand;
}