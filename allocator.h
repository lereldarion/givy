#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <atomic>

#include "alloc_parts.h"
#include "base_defs.h"
#include "utility.h"
#include "array.h"
#include "superpage_tracker.h"
#include "chain.h"

namespace Givy {
namespace Allocator {

	struct PageBlockHeader;
	class SuperpageBlock;
	struct MainHeap;
	class ThreadLocalHeap;

	struct UnusedBlock : public ForwardChain<UnusedBlock>::Element {
		Ptr ptr (void) const { return Ptr (this); }
	};

	using BlockFreeList = ForwardChain<UnusedBlock>;
	using ThreadRemoteFreeList = BlockFreeList::Atomic;
	using PageBlockUnusedList = QuickList<PageBlockHeader, 10>;

	/* Declarations */

	namespace Thresholds {
		/* Size constants that delimit the behaviour of the allocator.
		 *
		 * Smallest represents the smallest possible block of allocation.
		 * It cannot be smaller because we need every block to at least fit a pointer, when we chain
		 * them in a freelist.
		 *
		 * SmallMedium is user defined, and is not very constrained.
		 */
		constexpr size_t Smallest = Math::round_up_as_power_of_2 (sizeof (UnusedBlock));
		constexpr size_t SmallMedium = Math::round_up_as_power_of_2 (VMem::PageSize);
		// Threshold for MediumHigh is defined by superpageblock struct : later
	}

	namespace SizeClass {
		// List of PageBlockHeader for a given sizeclass
		struct PBListTag;
		using PBList = Chain<PageBlockHeader, PBListTag>;

		using Id = size_t;

		constexpr size_t min_sizeclass_log = Math::log_2_sup (Thresholds::Smallest);
		constexpr size_t max_sizeclass_log = Math::log_2_sup (Thresholds::SmallMedium);
		constexpr size_t nb_sizeclass = (max_sizeclass_log - min_sizeclass_log) + 1;

		Id id (size_t size) { return Math::log_2_sup (size) - min_sizeclass_log; }

		size_t block_size (Id sc_id) { return size_t (1) << (sc_id + min_sizeclass_log); }
		size_t page_block_size (Id sc_id) { return 1; }
		// TODO improve sizeclass info
	};

	enum class MemoryType {
		Small,  // Under page size
		Medium, // Between page and superpage size
		Huge,   // Above superpage size

		// Internal definitions
		Unused,  // Unused space, can be allocated
		Reserved // Used internally, not available for allocation
	};

	struct PageBlockHeader : public PageBlockUnusedList::Element, public SizeClass::PBList::Element {
		/* PageBlockHeader is used to represent Page blocks.
		 * Page blocks are the unit of memory in a SuperpageBlock.
		 * Page blocks have a type that represents the use of its memory.
		 *
		 * Every page has an associated PageBlockHeader, but only the first PageBlockHeader of a page
		 * block is actually used.
		 * All other PageBlockHeader of a page block will redirect to the first one with the head
		 * pointer.
		 *
		 * Any active PageBlockHeader of type Unused must be in the PageBlockUnusedList of its
		 * SuperpageBlock.
		 *
		 * An active PageBlockHeader of type Small is the only kind of PageBlockHeader that can be in a
		 * SizeClass::PBList.
		 * It will be in the list of its sizeclass if and only if it is neither full nor empty.
		 *
		 * Unused page blocks which are neighbours will be merged.
		 */
	public:
		/* Represent page block layout ; will be set by format()
		 */
		MemoryType type;        // Type of page block
		size_t block_page_nb;   // Size of page block
		PageBlockHeader * head; // pointer to active header representing the page block

		// Pointer to associated page (set by SuperpageBlock())
		Ptr page; // FIXME recompute everytime ?

		struct {
			size_t block_size;
			size_t max_nb_blocks;
			size_t nb_carved;
			size_t nb_unused;
			BlockFreeList unused;
		} small_blocks;

	public:
		size_t size (void) const { return block_page_nb; }
		void format (MemoryType type_, size_t pb_size, PageBlockHeader * head_);

		// Small blocks
		void configure_small_blocks (SizeClass::Id sc_id);

		Block take_small_block (void);
		void put_small_block (Ptr p);

		bool has_small_block (void) const;
		bool has_all_small_blocks (void) const;

#ifdef ASSERT_SAFE_ENABLED
		void print (void) const;
#endif
	};

	class SuperpageBlock : public Chain<SuperpageBlock>::Element {
		/* Superpage block (SPB) is the basic unit of memory allocation, and are always aligned to
		 * SuperpageSize.
		 * Superpage blocks are sequence of Superpages (size is configurable).
		 * The SuperpageBlock class data is put at each start of Superpage block, and holds metadata for
		 * the entire SPB.
		 *
		 * The first superpage of each SPB can be split into PageBlocks.
		 * All other superpages, if present, form a unique huge alloc.
		 *
		 * Memory structure of a superpage block :
		 * - superpage header (this class), followed by unused space to align to page boundaries,
		 * - sequence of pages for small & medium allocations,
		 * - last pages may be part of a huge_alloc,
		 * - a huge alloc will also span over the next superpages, which have no header (managed by this
		 * superpage).
		 *
		 * In other words :
		 * - a huge alloc SPB spans multiple superpages ; the last pages of the first superpage may be part of the huge alloc.
		 * - a non huge alloc SPB has one superpage that is completely dedicated to small/medium allocs (except for the reserved part).
		 * - a huge alloc SPB shrinks to a non huge page SPB if the huge alloc is destroyed.
		 *
		 * An empty SPB is always destroyed.
		 *
		 * ThreadLocalHeap lists:
		 * - SPB owned by a ThreadLocalHeap are always in the owned_superpage_blocks list
		 * TODO orphaned, optimization for full/not_full SPB
		 */
	private:
		/* Superpage block header.
		 *
		 * huge_alloc_pb_index indicates at which page index starts the huge alloc.
		 * If superpage_nb == 1 it must be equal to VMem::SuperpagePageNB.
		 * If equal to VMem::SuperpagePageNB and superpage_nb > 1, it means no pages in the first
		 * superpage are part of the huge alloc.
		 */
		std::atomic<ThreadLocalHeap *> owner;

		size_t superpage_nb;
		size_t huge_alloc_pb_index;

		PageBlockUnusedList unused;
		PageBlockHeader pbh_table[VMem::SuperpagePageNB];

	public:
		/* SuperpageBlock structure offsets */
		static const size_t HeaderSpacePages;
		static const size_t AvailablePages;

		/* Creation / destruction */
		SuperpageBlock (size_t superpage_nb_, size_t huge_alloc_page_nb, ThreadLocalHeap * owner_);
		~SuperpageBlock ();

		/* info */
		size_t size (void) const { return superpage_nb; }
		Ptr ptr (void) const { return Ptr (this); }
		bool completely_unused (void) const;

		static SuperpageBlock & from_pointer_in_first_superpage (Ptr inside);
		static SuperpageBlock & from_pbh (PageBlockHeader & pbh);
		static const SuperpageBlock & from_pbh (const PageBlockHeader & pbh);

		/* Huge alloc management */
		bool in_huge_alloc (Ptr p) const;
		Block huge_alloc_memory (void) const;
		void destroy_huge_alloc (void);

		/* Page block */
		PageBlockHeader * allocate_page_block (size_t page_nb, MemoryType type);
		void free_page_block (PageBlockHeader & pbh);

		Block page_block_memory (const PageBlockHeader & pbh) const;
		PageBlockHeader & page_block_header (Ptr p);
		bool all_page_blocks_unused (void) const;
		size_t page_block_index (const PageBlockHeader & pbh) const;

		/* Owner */
		ThreadLocalHeap * get_owner (void) const { return owner; }
		void disown (void) { owner = nullptr; }
		bool adopt (ThreadLocalHeap * adopter) {
			ThreadLocalHeap * expected = nullptr;
			return owner.compare_exchange_strong (expected, adopter);
		}

	private:
		/* Page block header formatting */
		void format_pbh (PageBlockHeader * from, PageBlockHeader * to, MemoryType type);
		void format_pbh (PageBlockHeader * from, size_t size, MemoryType type);
		void format_pbh (size_t from, size_t to, MemoryType type);

	public:
#ifdef ASSERT_SAFE_ENABLED
		void print (void) const;
#endif
	};

	const size_t SuperpageBlock::HeaderSpacePages =
	    Math::divide_up (sizeof (SuperpageBlock), VMem::PageSize);
	const size_t SuperpageBlock::AvailablePages =
	    VMem::SuperpagePageNB - SuperpageBlock::HeaderSpacePages;

	namespace Thresholds {
		constexpr size_t MediumHigh = SuperpageBlock::AvailablePages * VMem::PageSize;
	}

	struct MainHeap {
		using BootstrapAllocator = Parts::BackwardBumpPointer;

		GasLayout layout;
		BootstrapAllocator bootstrap_allocator;
		SuperpageTracker<BootstrapAllocator> superpage_tracker;

		/* Creation / destruction
		 */
		explicit MainHeap (const GasLayout & layout_);
		~MainHeap ();

		/* SuperpageBlock management
		 * - creation / destruction
		 * - finding header from any part of superpage block (requires superpage tracker)
		 */
		SuperpageBlock & create_superpage_block (ThreadLocalHeap * owner, size_t huge_alloc_size);
		void destroy_superpage_block (SuperpageBlock & spb);
		void destroy_superpage_huge_alloc (SuperpageBlock & spb);
		SuperpageBlock & containing_superpage_block (Ptr inside);

#ifdef ASSERT_SAFE_ENABLED
		void print (void) const;
#endif
	};

	class ThreadLocalHeap {
	private:
		MainHeap & main_heap;
		Chain<SuperpageBlock> owned_superpage_blocks;
		ThreadRemoteFreeList remote_freed_blocks;

		SizeClass::PBList pb_by_sizeclass[SizeClass::nb_sizeclass];

	public:
		/* Constructors and destructors are called on thread creation / destruction due to the use of
		 * ThreadLocalHeap as a global threal_local variable.
		 * A copy constructor is required by threal_local.
		 */
		explicit ThreadLocalHeap (MainHeap & main_heap_);
		~ThreadLocalHeap ();

		/* Interface
		 */
		Block allocate (size_t size, size_t align);
		void deallocate (Ptr ptr);
		void deallocate (Block blk);

	private:
		SuperpageBlock & create_superpage_block (size_t huge_alloc_size = 0);
		void destroy_superpage_block (SuperpageBlock & spb);
		PageBlockHeader & create_page_block (size_t nb_page, MemoryType type);
		void destroy_page_block (PageBlockHeader & pbh, SuperpageBlock & spb);
		Block allocate_small_block (size_t size);
		void destroy_small_block (Ptr ptr, PageBlockHeader & pbh, SuperpageBlock & spb);

		void thread_local_deallocate (Ptr ptr, SuperpageBlock & spb);
		void process_thread_remote_frees (void);

	public:
#ifdef ASSERT_SAFE_ENABLED
		void print (bool print_main_heap = true) const;
#endif
	};

	/* IMPL PageBlockHeader */

	void PageBlockHeader::format (MemoryType type_, size_t pb_size, PageBlockHeader * head_) {
		type = type_;
		block_page_nb = pb_size;
		head = head_;
	}

	void PageBlockHeader::configure_small_blocks (SizeClass::Id sc_id) {
		size_t block_size = SizeClass::block_size (sc_id);
		small_blocks = {block_size, size () * VMem::PageSize / block_size, 0, 0, BlockFreeList ()};
	}

	Block PageBlockHeader::take_small_block (void) {
		ASSERT_SAFE (has_small_block ());
		if (!small_blocks.unused.empty ()) {
			// Reuse unused block
			Ptr p = small_blocks.unused.front ().ptr ();
			small_blocks.unused.pop_front ();
			small_blocks.nb_unused--;
			return {p, small_blocks.block_size};
		} else {
			// Carve new block
			Ptr p = page + small_blocks.block_size * small_blocks.nb_carved;
			small_blocks.nb_carved++;
			return {p, small_blocks.block_size};
		}
	}

	void PageBlockHeader::put_small_block (Ptr p) {
		ASSERT_SAFE (page <= p);
		ASSERT_SAFE (p < page + VMem::PageSize * size ());
		UnusedBlock * blk = new (p.align (small_blocks.block_size)) UnusedBlock;
		small_blocks.unused.push_front (*blk);
		small_blocks.nb_unused++;
	}

	bool PageBlockHeader::has_small_block (void) const {
		return small_blocks.nb_unused > 0 || small_blocks.nb_carved < small_blocks.max_nb_blocks;
	}

	bool PageBlockHeader::has_all_small_blocks (void) const {
		return small_blocks.nb_unused == small_blocks.nb_carved;
	}

#ifdef ASSERT_SAFE_ENABLED
	void PageBlockHeader::print (void) const {
		if (type == MemoryType::Small)
			printf ("Small [szc=%zu,bs=%zu,cvd=%zu,un=%zu]\n", SizeClass::id (small_blocks.block_size),
			        small_blocks.block_size, small_blocks.nb_carved, small_blocks.nb_unused);
		else if (type == MemoryType::Medium)
			printf ("Medium [S=%zu]\n", size ());
		else if (type == MemoryType::Huge)
			printf ("Huge (start) [S=%zu]\n", size ());
		else if (type == MemoryType::Unused)
			printf ("Unused [S=%zu]\n", size ());
		else if (type == MemoryType::Reserved)
			printf ("Reserved [S=%zu]\n", size ());
		else
			printf ("<error> [Type=%ld]\n", static_cast<long> (type));
	}
#endif

	/* IMPL SuperpageBlock */

	SuperpageBlock::SuperpageBlock (size_t superpage_nb_, size_t huge_alloc_page_nb,
	                                ThreadLocalHeap * owner_)
	    : owner (owner_), superpage_nb (superpage_nb_) {
		DEBUG_TEXT ("[%p]SuperpageBlock()\n", this);

		// Compute huge page limit in first superpage
		ASSERT_SAFE (superpage_nb * VMem::SuperpagePageNB >= huge_alloc_page_nb + HeaderSpacePages);
		huge_alloc_pb_index = superpage_nb * VMem::SuperpagePageNB - huge_alloc_page_nb;

		// Set the PageBlockHeader page pointers
		for (size_t pb_index = 0; pb_index < VMem::SuperpagePageNB; ++pb_index)
			pbh_table[pb_index].page = ptr () + pb_index * VMem::PageSize;

		// Setup initial page blocks
		format_pbh (size_t (0), HeaderSpacePages, MemoryType::Reserved);
		format_pbh (HeaderSpacePages, huge_alloc_pb_index, MemoryType::Unused);
		format_pbh (huge_alloc_pb_index, VMem::SuperpagePageNB, MemoryType::Huge);
		unused.insert (pbh_table[HeaderSpacePages]); // Add initial unused page block to list
	}

	SuperpageBlock::~SuperpageBlock () {
#ifdef ASSERT_SAFE_ENABLED
		/* TODO for now, in SAFE build, allocator termination is an error if some blocks have not been
		 * deallocated.
		 */
		ASSERT_SAFE (unused.size () == AvailablePages);
		unused.take (AvailablePages); // make sure to empty unused list
#endif
		DEBUG_TEXT ("[%p]~SuperpageBlock()\n", this);
	}

	bool SuperpageBlock::completely_unused (void) const {
		return size () == 1 && all_page_blocks_unused ();
	}

	SuperpageBlock & SuperpageBlock::from_pointer_in_first_superpage (Ptr inside) {
		// Just use alignement
		return *inside.align (VMem::SuperpageSize).as<SuperpageBlock *> ();
	}
	SuperpageBlock & SuperpageBlock::from_pbh (PageBlockHeader & pbh) {
		// Pbh are stored in first superpage
		return from_pointer_in_first_superpage (Ptr (&pbh));
	}
	const SuperpageBlock & SuperpageBlock::from_pbh (const PageBlockHeader & pbh) {
		// Pbh are stored in first superpage
		return from_pointer_in_first_superpage (Ptr (&pbh));
	}

	bool SuperpageBlock::in_huge_alloc (Ptr p) const {
		ASSERT_SAFE (ptr () <= p);
		ASSERT_SAFE (p < ptr () + superpage_nb * VMem::SuperpageSize);
		return p > ptr () + huge_alloc_pb_index * VMem::PageSize;
	}

	Block SuperpageBlock::huge_alloc_memory (void) const {
		ASSERT_SAFE (superpage_nb > 1);
		size_t huge_alloc_page_nb = superpage_nb * VMem::SuperpagePageNB - huge_alloc_pb_index;
		return {ptr () + huge_alloc_pb_index * VMem::PageSize, huge_alloc_page_nb * VMem::PageSize};
	}

	void SuperpageBlock::destroy_huge_alloc (void) {
		/* Convert huge_alloc part of first superpage to unused space, if there is any.
		 * As the huge alloc trailing page block was formatted to a valid page block (but with Huge
		 * type), we can use free_page_block directly.
		 */
		if (huge_alloc_pb_index < VMem::SuperpagePageNB)
			free_page_block (pbh_table[huge_alloc_pb_index]);

		// Trim
		superpage_nb = 1;
	}

	PageBlockHeader * SuperpageBlock::allocate_page_block (size_t page_nb, MemoryType type) {
		ASSERT_SAFE (page_nb > 0);
		ASSERT_SAFE (page_nb < AvailablePages);

		PageBlockHeader * pbh = unused.take (page_nb);
		if (pbh) {
			size_t overflow_page_nb = pbh->size () - page_nb;
			if (overflow_page_nb > 0) {
				// cut overflow, configure and put back in list
				PageBlockHeader * overflow = pbh + page_nb;
				format_pbh (overflow, overflow_page_nb, MemoryType::Unused);
				unused.insert (*overflow);
			}
			// configure new alloc
			format_pbh (pbh, page_nb, type);
		}
		return pbh;
	}

	void SuperpageBlock::free_page_block (PageBlockHeader & pbh) {
		PageBlockHeader * start = &pbh;
		PageBlockHeader * end = start + pbh.size ();
		PageBlockHeader * const table_start = pbh_table;
		PageBlockHeader * const table_end = pbh_table + VMem::SuperpagePageNB;
		// Try merge with previous one
		if (start > table_start && start[-1].type == MemoryType::Unused) {
			PageBlockHeader * prev = start[-1].head;
			unused.remove (*prev);
			start = prev;
		}
		// Try merge with next one
		if (end < table_end && end->type == MemoryType::Unused) {
			PageBlockHeader * next = end;
			unused.remove (*next);
			end = next + next->size ();
		}
		// Put the merged PB in the list
		format_pbh (start, end, MemoryType::Unused);
		unused.insert (*start);
	}

	Block SuperpageBlock::page_block_memory (const PageBlockHeader & pbh) const {
		return {ptr () + page_block_index (pbh) * VMem::PageSize, pbh.size () * VMem::PageSize};
	}

	PageBlockHeader & SuperpageBlock::page_block_header (Ptr p) {
		ASSERT_SAFE (ptr () <= p);
		ASSERT_SAFE (p < ptr () + VMem::SuperpageSize);
		size_t pb_index = (p - ptr ()) / VMem::PageSize;
		return *pbh_table[pb_index].head;
	}

	bool SuperpageBlock::all_page_blocks_unused (void) const {
		// Test if unused quicklist contains every page
		return unused.size () == AvailablePages;
	}

	size_t SuperpageBlock::page_block_index (const PageBlockHeader & pbh) const {
		return &pbh - pbh_table;
	}

	void SuperpageBlock::format_pbh (PageBlockHeader * from, PageBlockHeader * to, MemoryType type) {
		size_t s = to - from;
		for (auto pbh = from; pbh < to; ++pbh)
			pbh->format (type, s, from);
	}
	void SuperpageBlock::format_pbh (PageBlockHeader * from, size_t size, MemoryType type) {
		format_pbh (from, from + size, type);
	}
	void SuperpageBlock::format_pbh (size_t from, size_t to, MemoryType type) {
		format_pbh (pbh_table + from, pbh_table + to, type);
	}

#ifdef ASSERT_SAFE_ENABLED
	void SuperpageBlock::print (void) const {
		printf ("S=%zu", size ());
		if (size () > 1)
			printf (" (huge alloc=%zu pages)", VMem::SuperpagePageNB * size () - huge_alloc_pb_index);
		printf ("\n");

		for (size_t i = 0; i < VMem::SuperpagePageNB; i += pbh_table[i].size ()) {
			printf ("\t[%zu-%zu]", i, i + pbh_table[i].size ());
			pbh_table[i].print ();
		}
	}
#endif

	/* IMPL MainHeap */

	MainHeap::MainHeap (const GasLayout & layout_)
	    : layout (layout_),
	      bootstrap_allocator (layout.start),
	      superpage_tracker (layout, bootstrap_allocator) {
		DEBUG_TEXT ("MainHeap()\n");
	}
	MainHeap::~MainHeap () { DEBUG_TEXT ("~MainHeap()\n"); }

	SuperpageBlock & MainHeap::create_superpage_block (ThreadLocalHeap * owner,
	                                                   size_t huge_alloc_size) {
		/* Compute sizes
		 * If huge_alloc_size is 0, allocates just one superpage
		 */
		size_t huge_alloc_page_nb = Math::divide_up (huge_alloc_size, VMem::PageSize);
		size_t superpage_nb = Math::divide_up (huge_alloc_page_nb + SuperpageBlock::HeaderSpacePages,
		                                       VMem::SuperpagePageNB);
		// Reserve, map, configure
		Ptr spb_start = layout.superpage (superpage_tracker.acquire (superpage_nb));
		VMem::map_checked (spb_start, superpage_nb * VMem::SuperpageSize);
		return *new (spb_start) SuperpageBlock (superpage_nb, huge_alloc_page_nb, owner);
	}

	void MainHeap::destroy_superpage_block (SuperpageBlock & spb) {
		Ptr spb_start = spb.ptr ();
		spb.~SuperpageBlock (); // manual call due to placement new construction
		superpage_tracker.release (layout.superpage_num (spb_start), spb.size ());
		VMem::unmap_checked (spb_start, spb.size () * VMem::SuperpageSize);
	}

	void MainHeap::destroy_superpage_huge_alloc (SuperpageBlock & spb) {
		// Destroy the trailing superpages
		size_t spb_size = spb.size ();
		ASSERT_STD (spb_size > 1);
		Ptr spb_start = spb.ptr ();
		superpage_tracker.trim (layout.superpage_num (spb_start), spb_size);
		VMem::unmap_checked (spb_start + VMem::SuperpageSize, (spb_size - 1) * VMem::SuperpageSize);

		// Update SPB header
		spb.destroy_huge_alloc ();
	}

	SuperpageBlock & MainHeap::containing_superpage_block (Ptr inside) {
		return *superpage_tracker.get_block_start (inside).as<SuperpageBlock *> ();
	}

#ifdef ASSERT_SAFE_ENABLED
	void MainHeap::print (void) const {
		printf ("Layout:\n");
		printf ("\tnodes (local node): %d (%d)\n", layout.nb_node, layout.local_node);
		printf ("\tsuperpage by node (total): %zu (%zu)\n", layout.superpage_by_node,
		        layout.superpage_total);
		printf ("\tnode area limits (sp index): [0");
		for (auto n = 0; n < layout.nb_node; ++n)
			printf (",%zu", layout.node_area_end_superpage_num (n));
		printf ("]\n");

		printf ("SuperpageTracker:\n");
		superpage_tracker.print ();
	}
#endif

	/* IMPL ThreadLocalHeap */

	ThreadLocalHeap::ThreadLocalHeap (MainHeap & main_heap_) : main_heap (main_heap_) {
		DEBUG_TEXT ("[%p]ThreadLocalHeap()\n", this);
	}

	ThreadLocalHeap::~ThreadLocalHeap () {
		DEBUG_TEXT ("[%p]~ThreadLocalHeap()\n", this);

		// Disown pages
		// TODO put in list ?
		for (auto & spb : owned_superpage_blocks)
			spb.disown ();
	}

	Block ThreadLocalHeap::allocate (size_t size, size_t align) {
		process_thread_remote_frees (); // TODO call less often

		/* Alignment support.
		 * Small allocations are aligned to the size of the sizeclass.
		 * All other allocations are aligned to pages.
		 * So we allow a simple support of up to page-size alignement, by using a size of at least
		 * requested alignement to answer the allocate request.
		 */
		ASSERT_STD (align <= VMem::PageSize);
		ASSERT_SAFE (Math::is_power_of_2 (align));
		size = std::max (size, align);

		if (size < Thresholds::SmallMedium) {
			// Small alloc
			return allocate_small_block (size);
		} else if (size < Thresholds::MediumHigh) {
			// Big alloc
			size_t page_nb = Math::divide_up (size, VMem::PageSize);
			PageBlockHeader & pbh = create_page_block (page_nb, MemoryType::Medium);
			return SuperpageBlock::from_pbh (pbh).page_block_memory (pbh);
		} else {
			// Huge alloc
			return create_superpage_block (size).huge_alloc_memory ();
		}
	}

	void ThreadLocalHeap::deallocate (Ptr ptr) {
		if (!main_heap.layout.in_local_area (ptr))
			return; // TODO node_remote free

		process_thread_remote_frees (); // TODO call less often

		SuperpageBlock & spb = main_heap.containing_superpage_block (ptr);
		ThreadLocalHeap * owner = spb.get_owner ();

		/* Adopt orphan superpage block.
		 * If it was orphan but adoption fails, it means another TLH adopted it, and we fall into the
		 * thread remote deallocation case.
		 */
		if (owner == nullptr && spb.adopt (this)) {
			// TODO add to TLH list ?
		}

		if (owner == this) {
			// Thread local deallocation
			thread_local_deallocate (ptr, spb);
		} else {
			// Node local Remote Thread deallocation

			/* Push the block on the remote TLH freelist
			 * The block is guaranteed to fit at least a UnusedBlock freelist link.
			 * TODO add SPB pointer to UnusedBlock to avoid computing it again ?
			 */
			UnusedBlock * blk = new (ptr) UnusedBlock;
			owner->remote_freed_blocks.push_front (*blk);
		}
	}

	void ThreadLocalHeap::deallocate (Block blk) {
		// TODO optimize for small/medium alloc, no call to SPT
		deallocate (blk.ptr);
	}

	SuperpageBlock & ThreadLocalHeap::create_superpage_block (size_t huge_alloc_size) {
		SuperpageBlock & spb = main_heap.create_superpage_block (this, huge_alloc_size);
		owned_superpage_blocks.push_back (spb);
		return spb;
	}

	void ThreadLocalHeap::destroy_superpage_block (SuperpageBlock & spb) {
		owned_superpage_blocks.remove (spb);
		main_heap.destroy_superpage_block (spb);
	}

	PageBlockHeader & ThreadLocalHeap::create_page_block (size_t nb_page, MemoryType type) {
		// Try to take from existing superpage blocks
		for (auto & spb : owned_superpage_blocks)
			if (PageBlockHeader * pbh = spb.allocate_page_block (nb_page, type)) {
				return *pbh;
			}
		// If failed : Take from a new superpage
		SuperpageBlock & spb = create_superpage_block ();
		PageBlockHeader * pbh = spb.allocate_page_block (nb_page, type);
		ASSERT_STD (pbh != nullptr);
		return *pbh;
	}

	void ThreadLocalHeap::destroy_page_block (PageBlockHeader & pbh, SuperpageBlock & spb) {
		spb.free_page_block (pbh);
		if (spb.completely_unused ())
			destroy_superpage_block (spb);
	}

	Block ThreadLocalHeap::allocate_small_block (size_t size) {
		auto sc_id = SizeClass::id (std::max (size, Thresholds::Smallest));

		// Create new page block if there is none available.
		if (pb_by_sizeclass[sc_id].empty ()) {
			auto & new_pbh = create_page_block (SizeClass::page_block_size (sc_id), MemoryType::Small);
			new_pbh.configure_small_blocks (sc_id);
			pb_by_sizeclass[sc_id].push_front (new_pbh);
		}

		// Pick block from current page block
		auto & pbh = pb_by_sizeclass[sc_id].front ();
		Block blk = pbh.take_small_block ();

		// Remove from list if empty
		if (!pbh.has_small_block ())
			pb_by_sizeclass[sc_id].pop_front ();

		return blk;
	}

	void ThreadLocalHeap::destroy_small_block (Ptr ptr, PageBlockHeader & pbh, SuperpageBlock & spb) {
		bool had_no_block = !pbh.has_small_block ();
		pbh.put_small_block (ptr);
		if (pbh.has_all_small_blocks ()) {
			SizeClass::PBList::unlink (pbh);
			destroy_page_block (pbh, spb);
		} else if (had_no_block) {
			// FIXME add to sizeclass list
		}
	}

	void ThreadLocalHeap::thread_local_deallocate (Ptr ptr, SuperpageBlock & spb) {
		if (spb.in_huge_alloc (ptr)) {
			// Huge alloc
			if (spb.all_page_blocks_unused ())
				main_heap.destroy_superpage_block (spb);
			else
				main_heap.destroy_superpage_huge_alloc (spb);
		} else {
			PageBlockHeader & pbh = spb.page_block_header (ptr);
			if (pbh.type == MemoryType::Small) {
				// Small alloc
				destroy_small_block (ptr, pbh, spb);
			} else if (pbh.type == MemoryType::Medium) {
				// Medium alloc
				destroy_page_block (pbh, spb);
			} else {
				// Unreachable
				ASSERT_SAFE_FAIL ("PageBlockHeader is neither Small nor Medium");
			}
		}
	}

	void ThreadLocalHeap::process_thread_remote_frees (void) {
		ForwardChain<UnusedBlock> unused_blocks = remote_freed_blocks.take_all ();
		for (auto it = unused_blocks.begin (); it != unused_blocks.end ();) {
			Ptr p = it->ptr ();
			it++; // Get next element before destroying the current element
			SuperpageBlock & spb = main_heap.containing_superpage_block (p);
			thread_local_deallocate (p, spb);
		}
	}

#ifdef ASSERT_SAFE_ENABLED
	void ThreadLocalHeap::print (bool print_main_heap) const {
		if (print_main_heap) {
			printf ("====== MainHeap ======\n");
			main_heap.print ();
		}

		printf ("====== ThreadLocalHeap [%p] ======\n", this);
		printf ("Owned SuperpageBlocks:\n");
		for (auto & spb : owned_superpage_blocks) {
			printf ("[%zu]", main_heap.layout.superpage_num (spb.ptr ()));
			spb.print ();
		}
		printf ("SizeClass lists:\n");
		for (SizeClass::Id i = 0; i < SizeClass::nb_sizeclass; ++i) {
			printf ("[%zu,bs=%zu]", i, SizeClass::block_size (i));
			for (auto & pbh : pb_by_sizeclass[i]) {
				auto & spb = SuperpageBlock::from_pbh (pbh);
				printf (" (%zu,%zu)", main_heap.layout.superpage_num (spb.ptr ()),
				        spb.page_block_index (pbh));
			}
			printf ("\n");
		}
	}
#endif
}
}

#endif