/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "vm/anon.h"
#include "vm/file.h"
#include "userprog/process.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/* page 구조체를 할당하기 위한 함수 */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;
	/* Check whether the upage is already occupied or not. */
	upage = pg_round_down(upage);

	if (spt_find_page(spt, upage) == NULL)
	{

		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* calloc과 malloc의 차이점은 calloc은 0으로 초기화함. sizeof(struct page)만큼 하나를 만든다. */
		/* calloc()을 통해 새로운 page 구조체를 생성하고, uninit_new()를 호출하여 해당 페이지를 uninit으로 만든다. uninit 페이지는 물리 메모리에 매핑된 적이 없는 페이지로, 페이지를 할당할 때는 무조건 uninit으로 만들어진다. 이후 물리 메모리에 할달될 때 비로소 인자로 전달된 type에 맞는 페이지가 된다.  */
		struct page *page = calloc(1, sizeof(struct page));
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			uninit_new(page, upage, init, type, aux, anon_initializer);
			break;
		case VM_FILE:
			uninit_new(page, upage, init, type, aux, file_backed_initializer);
			break;
		default:
			printf("vm_alloc_page_with_initialize: Unexpected page tyep.\n");
			goto err;
		}

		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, page))
		{
			goto err;
		}

		// /* Claim immediately if the page is the first stack page. */
		// if (VM_IS_STACK(type))
		// {

		// 	return vm_do_claim_page(page);
		// }

		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	/* TODO: Fill this function. */
	struct hash *pages = &spt->pages;
	struct hash_elem *e;
	struct page p;

	p.va = va;
	e = hash_find(pages, &p.hash_elem);

	return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED,
					 struct page *page UNUSED)
{
	return hash_insert(&spt->pages, &page->hash_elem) == NULL ? true : false;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame(void)
{
	struct frame *frame = calloc(1, sizeof(struct frame));

	if ((frame->kva = palloc_get_page(PAL_USER | PAL_ZERO)) == NULL)
		PANIC("TODO: if user pool memory is full, need to evict the frame.");

	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);

	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
	struct supplemental_page_table *spt = &thread_current()->spt;
	void *pg_aligned_addr = pg_round_down(addr);

	while(!spt_find_page(spt, pg_aligned_addr)){
		vm_alloc_page(VM_ANON | VM_MARKER_0, pg_aligned_addr, true);
		vm_claim_page(pg_aligned_addr);
		pg_aligned_addr += PGSIZE;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* f: page fault가 발생한 시점의 레지스터 값이 담겨있는 intr_frame
 * addr: page fault의 원인이 된 주소값
 * user: page fault가 user에 의한 것인지 아닌지에 대한 정보
 * write: page fault의 원인이 write를 하려고 했기 때문인지 아닌지에 대한 정보
 * not_present: addr이 접근 가능한 페이지인지 아닌지에 대한 정보  */
/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
						 bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
	struct page *page = NULL;
	uint64_t MAX_STACK = USER_STACK - (1 << 20);
	void *pg_aligned_addr = pg_round_down(addr);
	void *stack_pointer = NULL;
	/* physical page는 존재하나, writable 하지 않은 address에 write를 시도해서 일어난 fault인 경우, 할당하지 않고 즉시 false를 반환한다. */
	if ((!not_present) && (write))
		return false;
	
	if (user)
		stack_pointer = f->rsp;
	else
		stack_pointer = thread_current()->stack_pointer;

	/* supplemental page table에 존재하지 않는 page라면, false를 반환한다. */
	page = spt_find_page(spt, pg_round_down(addr));
	if (!page)
	{
		if (stack_pointer - 8 <= addr && MAX_STACK < addr && addr < USER_STACK){
			vm_stack_growth(pg_aligned_addr);
			page = spt_find_page(spt, pg_aligned_addr);
		}
		else {
			return false;
		}
		
	}
	return vm_do_claim_page(page);

	/* supplemental page table에 존재하는 page가 page fault를 발생시켰다면,
	 * 그 이유는 물리 메모리에 할당되지 못했기 때문이다.
	 * 따라서 해당 page를 물리 메모리에 할당시켜주는 방법으로
	 * page fault를 해결한 뒤 제어를 user process에게 다시 넘겨준다. */

}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va UNUSED)
{
	struct page *page = NULL;
	page = spt_find_page(&thread_current()->spt, pg_round_down(va));
	if (page)
	{
		return vm_do_claim_page(page);
	}
	return false;
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	struct hash *pages = &thread_current()->spt.pages;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page(thread_current()->pml4, page->va, frame->kva, true);

	// hash_insert(pages, &page->hash_elem);

	return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
/* userprog/process.c의 initd 함수로 새로운 프로세스가 시작하거나
 * process.c의 __do_fork로 자식 프로세스가 생성될 때 함수 호출 */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src)
{
	struct hash_iterator i;
	struct hash *h = &src->pages;

	hash_first(&i, h);
	while (hash_next(&i))
	{
		struct page *p = hash_entry(hash_cur(&i), struct page, hash_elem);
		enum vm_type type = page_get_type(p);

		void *va = p->va;
		bool writable = p->writable;

		if (p->operations->type == VM_UNINIT)
		{
			vm_initializer *init = p->uninit.init;
			struct aux *aux = (struct aux *)malloc(sizeof(struct aux));
			aux = p->uninit.aux;

			if (!vm_alloc_page_with_initializer(type, va, writable, init, aux))
			{
				return false;
			}
		}
		else
		{
			if (!vm_alloc_page(type, va, writable))
			{
				return false;
			}
			if (!vm_claim_page(va))
			{
				return false;
			}
			memcpy(va, p->frame->kva, PGSIZE);
		}
	}
	return true;
}

void clear_func(struct hash_elem *elem, void *aux)
{
	struct page *page = hash_entry(elem, struct page, hash_elem);
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	// struct hash_iterator i;
	// struct hash *h = &spt->pages;

	// hash_first(&i, h);
	// while (hash_next(&i)){
	// 	struct page *p = hash_entry(hash_cur(&i), struct page, hash_elem);
	// 	destroy(p);
	// }
	// hash_destroy(h, NULL);
	// hash_init(h, page_hash, page_less, NULL);
	hash_clear(&spt->pages, clear_func);
}

unsigned page_hash(const struct hash_elem *p_, void *aux)
{
	const struct page *p = hash_entry(p_, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
}

bool page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux)
{
	const struct page *a = hash_entry(a_, struct page, hash_elem);
	const struct page *b = hash_entry(b_, struct page, hash_elem);

	return a->va < b->va;
}
