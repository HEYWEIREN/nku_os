#include <pmm.h>
#include <list.h>
#include <buddy_pmm.h>

//order 表示块的阶数
#define BUDDY_MAX_ORDER 31
// Private flag: allocated block head; property stores the original request.
#define BUDDY_ALLOCATED (1UL << 2)//把 flags 的第 2 位作为“已分配块首页”的标记

//按阶数建立多条链表
struct buddy_area {
    //free_list[i]	保存所有大小为 \(2^i\) 页的空闲块
    list_entry_t free_list[BUDDY_MAX_ORDER + 1];
    struct Page *pages;//这片管理区域的描述符数组起点
    size_t count;//描述符数量，即管理范围内的页数
    size_t first_ppn;//pages[0] 对应的物理页号
    size_t nr_free;
};

static struct buddy_area buddy_area;

static size_t
buddy_size(unsigned int order) {
    return (size_t)1 << order;
}

//寻找能够容纳 n 页的最小阶数
static unsigned int
buddy_order(size_t n) {
    unsigned int order = 0;
    while (order < BUDDY_MAX_ORDER && buddy_size(order) < n) {
        order ++;
    }
    return order;
}


static void
buddy_area_init(struct buddy_area *area, struct Page *base,
                size_t count, size_t first_ppn) {
    for (unsigned int i = 0; i <= BUDDY_MAX_ORDER; i ++) {
        list_init(&area->free_list[i]);
    }
    area->pages = base;
    area->count = count;
    area->first_ppn = first_ppn;
    area->nr_free = 0;
}

static void
buddy_add(struct buddy_area *area, struct Page *base, unsigned int order) {
    base->property = buddy_size(order);
    SetPageProperty(base);
    list_add(&area->free_list[order], &base->page_link);
}

static void
buddy_remove(struct Page *base) {
    list_del_init(&base->page_link);
    ClearPageProperty(base);
    base->property = 0;
}

//如何找到唯一的伙伴
// Use absolute physical page numbers so all blocks have physical alignment.
static struct Page *
buddy_partner(struct buddy_area *area, struct Page *base, unsigned int order) {
    size_t ppn = area->first_ppn + (base - area->pages);
    size_t other = ppn ^ buddy_size(order);
    if (other < area->first_ppn) {
        return NULL;
    }
    size_t index = other - area->first_ppn;
    if (index >= area->count || buddy_size(order) > area->count - index) {
        return NULL;
    }
    return area->pages + index;
}

//逐级尝试合并
static void
buddy_merge(struct buddy_area *area, struct Page *base, unsigned int order) {
    while (order < BUDDY_MAX_ORDER) {
        struct Page *other = buddy_partner(area, base, order);
        if (other == NULL || !PageProperty(other) ||
            other->property != buddy_size(order)) {
            break;
        }
        buddy_remove(other);
        if (other < base) {
            base = other;
        }
        order ++;
    }
    buddy_add(area, base, order);
}

//把任意可用区切成合法的伙伴块
static void
buddy_area_memmap(struct buddy_area *area, struct Page *base, size_t n) {
    assert(n > 0);
    assert(base >= area->pages && base < area->pages + area->count);
    assert(n <= area->count - (base - area->pages));
    for (struct Page *p = base; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    area->nr_free += n;
    while (n > 0) {
        size_t ppn = area->first_ppn + (base - area->pages);
        unsigned int order = 0;
        while (order < BUDDY_MAX_ORDER && buddy_size(order + 1) <= n &&
               (ppn & (buddy_size(order + 1) - 1)) == 0) {
            order ++;
        }
        size_t size = buddy_size(order);
        buddy_merge(area, base, order);
        base += size;
        n -= size;
    }
}

//找到够大的块，再逐级拆分
static struct Page *
buddy_area_alloc(struct buddy_area *area, size_t n) {
    assert(n > 0);
    if (n > area->nr_free || n > buddy_size(BUDDY_MAX_ORDER)) {
        return NULL;
    }
    unsigned int order = buddy_order(n), found = order;
    while (found <= BUDDY_MAX_ORDER && list_empty(&area->free_list[found])) {
        found ++;
    }
    if (found > BUDDY_MAX_ORDER) {
        return NULL;
    }
    struct Page *page = le2page(list_next(&area->free_list[found]), page_link);
    buddy_remove(page);
    while (found > order) {
        found --;
        buddy_add(area, page + buddy_size(found), found);
    }
    page->flags |= BUDDY_ALLOCATED;
    page->property = n;
    area->nr_free -= buddy_size(order);
    return page;
}

static void
buddy_area_free(struct buddy_area *area, struct Page *base, size_t n) {
    assert(n > 0 && n <= buddy_size(BUDDY_MAX_ORDER));
    assert(base >= area->pages && base < area->pages + area->count);
    assert((base->flags & BUDDY_ALLOCATED) && base->property == n);
    unsigned int order = buddy_order(n);
    size_t size = buddy_size(order);
    assert(size <= area->count - (base - area->pages));
    for (struct Page *p = base; p != base + size; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    area->nr_free += size;
    buddy_merge(area, base, order);
}

static void
buddy_init(void) {
    // page_init establishes pages/npage after the manager's init callback.
    buddy_area_init(&buddy_area, NULL, 0, 0);
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    if (buddy_area.pages == NULL) {
        assert(npage > nbase);
        buddy_area_init(&buddy_area, pages, npage - nbase, nbase);
    }
    buddy_area_memmap(&buddy_area, base, n);
}

static struct Page *
buddy_alloc_pages(size_t n) {
    return buddy_area_alloc(&buddy_area, n);
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    buddy_area_free(&buddy_area, base, n);
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_area.nr_free;
}

// Tests use private descriptors and the same allocator core, not physical RAM.
#define BUDDY_TEST_PAGES 64
static struct Page test_pages[BUDDY_TEST_PAGES];
static struct buddy_area test_area;

static void
buddy_test_init(size_t first_ppn) {
    buddy_area_init(&test_area, test_pages, BUDDY_TEST_PAGES, first_ppn);
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i ++) {
        test_pages[i].flags = 0;
        test_pages[i].property = 0;
        set_page_ref(&test_pages[i], 0);
        SetPageReserved(&test_pages[i]);
    }
}

// Independently mark free pages to detect overlaps, cycles and wrong totals.
static void
buddy_test_check(void) {
    bool seen[BUDDY_TEST_PAGES] = {0};
    size_t total = 0;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &test_area.free_list[order], *le = head;
        size_t blocks = 0, size = buddy_size(order);
        while ((le = list_next(le)) != head) {
            assert(++blocks <= BUDDY_TEST_PAGES);
            assert(list_prev(list_next(le)) == le);
            assert(list_next(list_prev(le)) == le);
            struct Page *p = le2page(le, page_link);
            assert(p >= test_pages && p < test_pages + BUDDY_TEST_PAGES);
            size_t index = p - test_pages;
            assert(size <= BUDDY_TEST_PAGES - index);
            assert(((test_area.first_ppn + index) & (size - 1)) == 0);
            assert(PageProperty(p) && p->property == size);
            for (size_t i = 0; i < size; i ++) {
                assert(!seen[index + i]);
                assert(!PageReserved(p + i));
                assert(!((p + i)->flags & BUDDY_ALLOCATED));
                assert(i == 0 || !PageProperty(p + i));
                seen[index + i] = 1;
            }
            struct Page *other = buddy_partner(&test_area, p, order);
            assert(order == BUDDY_MAX_ORDER || other == NULL ||
                   !PageProperty(other) || other->property != size);
            total += size;
        }
    }
    assert(total == test_area.nr_free);
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i ++) {
        assert(!PageProperty(&test_pages[i]) || seen[i]);
    }
}

static void
buddy_check(void) {
    struct Page *blocks[BUDDY_TEST_PAGES];
    size_t requests[] = {1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 64};
    buddy_test_init(0);
    assert(buddy_area_alloc(&test_area, 1) == NULL);
    buddy_area_memmap(&test_area, test_pages, BUDDY_TEST_PAGES);
    buddy_test_check();

    // Exact powers, rounding, full-arena allocation and overflow-safe failure.
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i ++) {
        size_t n = requests[i], size = 1;
        while (size < n) {
            size *= 2;
        }
        struct Page *p = buddy_area_alloc(&test_area, n);
        assert(p == test_pages);
        assert(test_area.nr_free == BUDDY_TEST_PAGES - size);
        buddy_test_check();
        buddy_area_free(&test_area, p, n);
        assert(test_area.nr_free == BUDDY_TEST_PAGES);
        buddy_test_check();
    }
    assert(buddy_area_alloc(&test_area, 65) == NULL);
    assert(buddy_area_alloc(&test_area, (size_t)-1) == NULL);
    assert(test_area.nr_free == BUDDY_TEST_PAGES);
    buddy_test_check();

    // Exhaustion, uniqueness and fragmented failure despite enough free pages.
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i ++) {
        blocks[i] = buddy_area_alloc(&test_area, 1);
        assert(blocks[i] != NULL);
        for (size_t j = 0; j < i; j ++) {
            assert(blocks[i] != blocks[j]);
        }
        buddy_test_check();
    }
    assert(test_area.nr_free == 0);
    assert(buddy_area_alloc(&test_area, 1) == NULL);
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i ++) {
        if ((blocks[i] - test_pages) % 2 == 0) {
            buddy_area_free(&test_area, blocks[i], 1);
        }
    }
    assert(test_area.nr_free == 32);
    assert(buddy_area_alloc(&test_area, 2) == NULL);
    buddy_test_check();
    for (size_t i = BUDDY_TEST_PAGES; i > 0; i --) {
        if ((blocks[i - 1] - test_pages) % 2 != 0) {
            buddy_area_free(&test_area, blocks[i - 1], 1);
            buddy_test_check();
        }
    }
    struct Page *p = buddy_area_alloc(&test_area, 64);
    assert(p == test_pages);
    buddy_area_free(&test_area, p, 64);

    // Mixed sizes and repeated reuse; compare complete rounded allocations.
    size_t mixed[] = {3, 5, 1, 9, 2};
    size_t sizes[] = {4, 8, 1, 16, 2};
    unsigned int release[] = {2, 4, 0, 3, 1};
    for (unsigned int round = 0; round < 16; round ++) {
        size_t used = 0;
        for (size_t i = 0; i < 5; i ++) {
            blocks[i] = buddy_area_alloc(&test_area, mixed[i]);
            assert(blocks[i] != NULL);
            assert((blocks[i] - test_pages) % sizes[i] == 0);
            for (size_t j = 0; j < i; j ++) {
                assert(blocks[i] + sizes[i] <= blocks[j] ||
                       blocks[j] + sizes[j] <= blocks[i]);
            }
            used += sizes[i];
            assert(test_area.nr_free == BUDDY_TEST_PAGES - used);
            buddy_test_check();
        }
        for (size_t i = 0; i < 5; i ++) {
            unsigned int j = release[i];
            buddy_area_free(&test_area, blocks[j], mixed[j]);
            used -= sizes[j];
            assert(test_area.nr_free == BUDDY_TEST_PAGES - used);
            buddy_test_check();
        }
        p = buddy_area_alloc(&test_area, 64);
        assert(p == test_pages);
        buddy_area_free(&test_area, p, 64);
    }

    // Adjacent blocks [4,8) and [8,12) are not buddies.
    buddy_test_init(0);
    buddy_area_memmap(&test_area, test_pages + 4, 8);
    assert(buddy_area_alloc(&test_area, 8) == NULL);
    buddy_test_check();

    // Unaligned physical origin, odd length, separate maps and a reserved hole.
    buddy_test_init(3);
    buddy_area_memmap(&test_area, test_pages + 1, 13);
    buddy_area_memmap(&test_area, test_pages + 16, 17);
    buddy_test_check();
    for (size_t i = 0; i < 30; i ++) {
        blocks[i] = buddy_area_alloc(&test_area, 1);
        assert(blocks[i] != NULL);
        size_t index = blocks[i] - test_pages;
        assert((index >= 1 && index < 14) || (index >= 16 && index < 33));
        for (size_t j = 0; j < i; j ++) {
            assert(blocks[i] != blocks[j]);
        }
    }
    assert(buddy_area_alloc(&test_area, 1) == NULL);
    for (size_t i = 0; i < 30; i ++) {
        buddy_area_free(&test_area, blocks[(i * 7) % 30], 1);
        buddy_test_check();
    }
    assert(test_area.nr_free == 30);
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i ++) {
        if (!(i >= 1 && i < 14) && !(i >= 16 && i < 33)) {
            assert(PageReserved(&test_pages[i]));
        }
    }

    // Adjacent maps supplied out of order must coalesce.
    buddy_test_init(0);
    buddy_area_memmap(&test_area, test_pages + 32, 32);
    buddy_area_memmap(&test_area, test_pages, 32);
    buddy_test_check();
    p = buddy_area_alloc(&test_area, 64);
    assert(p == test_pages);
    buddy_area_free(&test_area, p, 64);
    buddy_test_check();
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
