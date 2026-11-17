#ifndef MYOS_INSTALLER_H
#define MYOS_INSTALLER_H
enum { INSTALL_PROBE, INSTALL_LIST, INSTALL_PLAN, INSTALL_PREPARE, INSTALL_COMMIT, INSTALL_ABORT };
enum { INSTALL_IDLE, INSTALL_PLANNED, INSTALL_COPYING, INSTALL_DONE };
struct disk_install_info {
    char device[12],model[48];
    unsigned sectors,root_start,root_sectors,token,state;
};
_Static_assert(sizeof(struct disk_install_info)==80,"installer ABI");
#define INSTALL_PAYLOAD "/boot/install/payload.bin"
#define INSTALL_TARGETS 4u
int disk_install(unsigned op,unsigned index,struct disk_install_info *info);
#endif
