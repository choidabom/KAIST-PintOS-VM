/* anonymous page를 위한 기능을 제공합니다. (vm_type = VM_ANON)*/
#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page
{
    vm_initializer *init;
    enum vm_type type;
    void *aux;
    bool (*page_initializer)(struct page *, enum vm_type, void *kva);
};

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif
