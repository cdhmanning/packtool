#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <zlib.h>


#define PACK_HDR_MAGIC		0x4B434150
#define PACK_TRAILER_MAGIC	0x5041434B

#define PACK_BLOCK_SIZE		512

#define PACK_HEADER_SIZE	1024
#define PACK_LABEL_LEN	15

struct pack_item {
	uint32_t flags;
	char     label[PACK_LABEL_LEN +1];
	uint32_t load_addr;
	uint32_t offset;
	uint32_t size;
	uint32_t load_size;
	uint32_t data_crc;
};

struct pack_header {
	uint32_t magic;
	uint32_t version; /** Version of the pack header format */
	uint32_t n_items;
	uint32_t pack_size; /** Size of whole pack */
	uint32_t revision; /** Revision of this pack, for comparing two packs */
	uint32_t hdr_crc;
	struct pack_item items[0];
};

union pack_header_block {
	uint32_t as_words [PACK_HEADER_SIZE/4];
	struct pack_header hdr;
};

#define N_PACK_ITEMS \
		((sizeof(union pack_header_block) - sizeof(struct pack_header))\
			/sizeof(struct pack_item))

#define ALIGN_SIZE	PACK_BLOCK_SIZE
#define ALIGN_UP(x) \
		(((x) + ALIGN_SIZE - 1) & (~(ALIGN_SIZE - 1)))

static union pack_header_block header;

struct item_descriptor {
	char *label;
	char * file_name;
	unsigned flags;
	unsigned load_addr;
};

static struct item_descriptor item_descriptors[N_PACK_ITEMS];
static int n_items;

static char *write_path;
static char *list_path;
static unsigned fw_revision;
static int pack_handle;

static void header_init(void)
{
	struct pack_header *hdr;

	memset(&header, 0, sizeof(header));
	hdr = &header.hdr;
	hdr->magic = PACK_HDR_MAGIC;
	hdr->version = 1;
	hdr->revision = fw_revision;
}

static void recalc_header_crc(void)
{
	header.hdr.hdr_crc = 0;
	header.hdr.hdr_crc = crc32(0, (unsigned char *)&header, sizeof (header));
}

static void header_finalise(void)
{
	int pack_size;

	pack_size = lseek(pack_handle, 0, SEEK_END);
	/* Add the size of the trailer (ie. the size of the header) */
	pack_size += sizeof(header);
	header.hdr.pack_size = pack_size;
	recalc_header_crc();
}

static void change_header_to_trailer(void)
{
	header.hdr.magic = PACK_TRAILER_MAGIC;
	recalc_header_crc();
}

static int add_item_to_header(struct pack_item *item)
{
	if (header.hdr.n_items < N_PACK_ITEMS) {
		header.hdr.items[header.hdr.n_items] = *item;
		header.hdr.n_items++;
		return 0;
	}
	return -1;
}

static int add_item(char *label, unsigned flags, unsigned load_addr, char *file_name)
{
	int in_handle;
	unsigned char buffer[PACK_BLOCK_SIZE];
	int copied;
	struct pack_item item;

	memset(&item, 0, sizeof(item));

	if (strlen(label) > PACK_LABEL_LEN) {
		fprintf(stderr, "Label \"%s\" is longer than %d characters\n",
			label, PACK_LABEL_LEN);
		return -1;
	}

	item.flags = flags;
	strcpy(item.label, label);
	item.load_addr = load_addr;
	item.offset = lseek(pack_handle, 0, SEEK_CUR);

	in_handle = open(file_name, O_RDONLY);

	if (in_handle < 0) {
		perror("opening file");
		fprintf(stderr, "File %s not found\n", file_name);
		return -1;
	}

	item.size = lseek(in_handle, 0, SEEK_END);
	lseek(in_handle, 0, SEEK_SET);

	if (item.size <= 0) {
		perror("finding file size");
		close(in_handle);
		return -1;
	}

	item.load_size = ALIGN_UP(item.size);

	item.data_crc = 0;
	for (copied = 0; copied < item.load_size; copied+= PACK_BLOCK_SIZE) {
		memset(buffer, 0, sizeof(buffer));
		read(in_handle, buffer, sizeof(buffer));
		item.data_crc = crc32(item.data_crc, buffer, sizeof(buffer));
		write(pack_handle, buffer, sizeof(buffer));
	}

	close(in_handle);

	return add_item_to_header(&item);
}

static void print_header(FILE *stream)
{
	int i;

	fprintf(stream, "Pack file info\n");
	fprintf(stream, "magic....... 0x%08x\n", header.hdr.magic);
	fprintf(stream, "version..... %d\n", header.hdr.version);
	fprintf(stream, "revision.....%d\n", header.hdr.revision);
	fprintf(stream, "pack size... %d\n", header.hdr.pack_size);
	fprintf(stream, "n_items..... %d\n", header.hdr.n_items);
	fprintf(stream, "crc......... 0x%08x\n", header.hdr.hdr_crc);
	for (i = 0; i < header.hdr.n_items; i++) {
		struct pack_item *item = &header.hdr.items[i];

		fprintf(stream,
"%2d: \"%s\": flags:0x%08x, load 0x%08x, offs 0x%08x, size 0x%08x, loadsize 0x%08x, crc 0x%08x\n",
			i, item->label, item->flags, item->load_addr, item->offset,
			item->size, item->load_size, item->data_crc);
	}
}


static void bad_args(void)
{
	fprintf(stderr,"usage: pack_tool [options]\n"
		"   -l file_name                        list existing pack file\n"
		"   -w file_name                        write new pack file\n"
		"   -i flags:load_addr:label:file_name   add pack item (needs writing) - max %d items\n"
		"   -r revision                         revision number (needs writing)\n",
		(int)N_PACK_ITEMS
		);
	exit(1);
}


static void add_item_descriptor(char *in_str)
{
	char *token;
	char *saveptr;
	char *str = strdup(in_str);
	unsigned flags;
	unsigned load_addr;
	char *label;
	char *fname;

	if (n_items >= N_PACK_ITEMS) {
		fprintf(stderr,"Too many items\n");
		bad_args();
	}

	token = strtok_r(str,":", &saveptr);

	if (!token)
		goto token_error;

	flags = strtoul(token, NULL, 0);

	token = strtok_r(NULL,":", &saveptr);

	if (!token)
		goto token_error;

	load_addr = strtoul(token, NULL, 0);

	label = strtok_r(NULL, ":", &saveptr);
	fname = strtok_r(NULL, ":", &saveptr);

	if (!label || !fname)
		goto token_error;

	item_descriptors[n_items].flags = flags;
	item_descriptors[n_items].load_addr = load_addr;
	item_descriptors[n_items].label = strdup(label);
	item_descriptors[n_items].file_name = strdup(fname);
	fprintf(stderr, "Add item %d: %s %x %x\n", n_items, label, flags, load_addr);
	n_items++;

	free(str);
	return;

token_error:
	fprintf(stderr,"Could not parse item descriptor \"%s\"\n",
			in_str);
	free(str);
	bad_args();
}



static void parse_args(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "i:l:w:r:")) !=-1) {
		switch(c) {
		case 'r':
			fw_revision = atoi(optarg);
			break;
		case 'i':
			add_item_descriptor(optarg);
			break;
		case 'l':
			list_path = strdup(optarg);
			break;
		case 'w':
			write_path = strdup(optarg);
			break;
		default:
			bad_args();
		}
	}
}

static void verify_checksums(union pack_header_block *header, unsigned char *buffer)
{
	uint32_t tmp_crc32;
	uint32_t calc_crc32;
	int i;
	int data_crcs_ok = 1;

	tmp_crc32 = header->hdr.hdr_crc;
	header->hdr.hdr_crc = 0;
	calc_crc32 = crc32(0, (unsigned char *)header, sizeof (*header));
	header->hdr.hdr_crc = tmp_crc32;

	if (tmp_crc32 != calc_crc32) {
		fprintf(stderr, "Header checksum does not match\n");
		exit(1);
	}

	fprintf(stderr, "Header OK\n");

	for (i = 0; i < header->hdr.n_items; i++) {
		struct pack_item *item= &header->hdr.items[i];

		calc_crc32 = crc32(0, buffer + item->offset,
				   item->load_size);
		if (calc_crc32 != item->data_crc) {
			fprintf(stderr,
				"Item %d crc does not match 0x%08x 0x%08x\n",
				i, item->data_crc, calc_crc32);
			data_crcs_ok = 0;
		}
	}

	if (data_crcs_ok)
		fprintf(stderr,"Data crcs OK\n");
}

static void do_list(void)
{
	int fsize;
	unsigned char *buffer;

	pack_handle = open(list_path, O_RDONLY);

	if (pack_handle < 0) {
		perror("Loading pack file");
		bad_args();
	}

	fsize = lseek(pack_handle, 0, SEEK_END);
	printf("pack file size is %d bytes\n", fsize);
	lseek(pack_handle, 0, SEEK_SET);

	buffer = malloc(fsize);
	if (!buffer) {
		fprintf(stderr, "Could not allocate pack buffer\n");
		exit(1);
	}

	if (read(pack_handle, buffer, fsize) != fsize) {
		fprintf(stderr, "Error reading buffer\n");
		exit(1);
	}

	memcpy(&header, buffer, sizeof(header));
	print_header(stderr);
	verify_checksums(&header, buffer);
	close(pack_handle);
	free(buffer);
}

static void do_write(void)
{
	int written;
	int i;
	int ret;

	pack_handle = open(write_path, O_RDWR | O_TRUNC | O_CREAT, 0666);

	if (pack_handle < 0) {
		perror("Creating pack file");
		bad_args();
	}

	header_init();

	/*
	 * Write the header. This is not the final header data and
	 * needs to be rewritten.
	 */
	written = write(pack_handle, &header, sizeof(header));
	if (written != sizeof(header))
	{
		perror("Writing pack file");
		exit(1);
	}

	/* Write the items. */
	for (i = 0; i < n_items; i++) {
		struct item_descriptor *item = &item_descriptors[i];

		ret = add_item(item->label, item->flags, item->load_addr, item->file_name);
		
		if (ret!= 0) {
			fprintf(stderr, "Failed to write item %s\n", item->file_name);
			exit(1);
		}
	}

	header_finalise();

	/* Rewrite the header. */
	lseek(pack_handle, 0, SEEK_SET);
	written = write(pack_handle, &header, sizeof(header));
	if (written != sizeof(header))
	{
		perror("Writing pack file");
		exit(1);
	}

	/* Seek to end and write trailer. */
	change_header_to_trailer();
	lseek(pack_handle, 0, SEEK_END);
	written = write(pack_handle, &header, sizeof(header));
	if (written != sizeof(header))
	{
		perror("Writing pack file");
		exit(1);
	}

	close(pack_handle);

	do_list();
}


int main(int argc, char *argv[])
{
	parse_args(argc, argv);

	if((!list_path && !write_path) || (list_path && write_path)) {
		fprintf(stderr, "Need one of -l or -w\n");
		bad_args();
	}

	if (list_path)
		do_list();
	else if (write_path) {
		list_path = write_path;
		do_write();
	}
	return 0;
}
