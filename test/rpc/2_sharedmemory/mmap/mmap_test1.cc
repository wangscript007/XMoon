#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <iostream>
#include <unistd.h>

typedef struct
{
    char name;
    size_t age;
} PeopleInfo;

// map a normal file as shared mem:　
int main(int argc, char **argv)
{
    int fd;
    PeopleInfo *pmap;

    fd = open(argv[1], O_CREAT | O_RDWR | O_TRUNC, 00777);
    lseek(fd, sizeof(PeopleInfo) * 5 - 1, SEEK_SET);
    write(fd, "", 1);
    pmap = (PeopleInfo *)mmap(NULL, sizeof(PeopleInfo) * 10, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pmap < 0)
    {
        std::cout << "Failed to create to mmap ." << std::endl;
        return -1;
    }
    close(fd);

    char temp = 'a';
    for (size_t i = 0; i < 10; i++)
    {
        pmap[i].name = temp++;
        pmap[i].age = 20 + i;
    }
    if (munmap(pmap, sizeof(PeopleInfo) * 10) < 0)
    {
        std::cout << "Failed to delete to mmap ." << std::endl;
        return -2;
    }
    return 0;
}
