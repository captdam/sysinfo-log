#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>

#define FILE_SIZE 4096 // 4096 or 65536: Better align with memory page size and disk cluster size
#define DATA_COUNT 255 // 255 or 4095: Cross check with FILE_SIZE, FILE_SIZE = 16 + 16 * DATA_COUNT
#define DATA_UNITSIZE ((uint64_t)(64 * 4096))


typedef struct {
	uint16_t data_wp, data_total;
	uint16_t ram_total, swap_total;
	uint16_t data_blocksize;
	uint16_t _pad[3];
} header_t;

typedef struct {
	uint32_t uptime;
	uint16_t process;
	uint8_t load1, load5;
	uint16_t ram_free, ram_shared, ram_buffer, swap_free;
} data_t;

typedef struct {
	header_t header;
	data_t data[DATA_COUNT];
} buffer_t;

int main(int argc, char** argv) {
	static_assert(sizeof(data_t) == 16, "Data size must be 16");
	static_assert(sizeof(header_t) == 16, "Header size must be 16");
	static_assert(sizeof(buffer_t) == FILE_SIZE, "Buffer size mismatch with data count");
	static_assert(DATA_UNITSIZE / 4096 <= 65535, "Data unit cannot fit into uint16_t");

	struct sysinfo sys_info;
	if (sysinfo(&sys_info)) {
		fprintf(stderr, "Cannot get sysinfo err: %d.\n", errno);
		return -1;
	}
	uint64_t ram_total = (uint64_t)(sys_info.totalram) * sys_info.mem_unit, ram_write = ram_total / DATA_UNITSIZE;
	uint64_t swap_total = (uint64_t)(sys_info.totalswap) * sys_info.mem_unit, swap_write = swap_total / DATA_UNITSIZE;
	if ( ram_write > (uint16_t)-1 || swap_write > (uint16_t)-1 ) {
		fprintf(stderr, "DATA_UNITSIZE (%"PRIu64") too small, allow max %"PRIu64" - RAM needs %"PRIu64" Swap needs %"PRIu64"\n", DATA_UNITSIZE, (uint64_t)(DATA_UNITSIZE * (uint16_t)-1), ram_total, swap_total);
		return -1;
	}

	if (argc != 2) {
		fprintf(stderr, "Buffer filename not provided.\n");
		return -1;
	}
	int file = open(argv[1],
		O_RDWR | O_CREAT,
		S_IRUSR|S_IWUSR | S_IRGRP | S_IROTH
	);
	if (file < 0) {
		fprintf(stderr, "Cannot open file err: %d.\n", errno);
		return -1;
	}
	if (flock(file, LOCK_EX) < 0) {
		fprintf(stderr, "Cannot lock file err: %d.\n", errno);
		return -1;
	}
	if (ftruncate(file, sizeof(buffer_t)) < 0) {
		fprintf(stderr, "Cannot allocate buffer file err: %d.\n", errno);
		return -1;
	}

	buffer_t* buffer = mmap(NULL, sizeof(buffer_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, file, 0);
	header_t* header = &(buffer->header);

	if (header->data_total == 0) {
		fprintf(stderr, "Create file.\n");
		header->data_wp		= 0;
		header->data_total	= DATA_COUNT;
		header->data_blocksize	= DATA_UNITSIZE / 4096; // of 4k page
		header->ram_total	= ram_total / DATA_UNITSIZE;
		header->swap_total	= swap_total / DATA_UNITSIZE;
	}

	float load1 = 250.0f * sys_info.loads[0] / get_nprocs() / (1<<SI_LOAD_SHIFT); // out of 250
	float load5 = 250.0f * sys_info.loads[1] / get_nprocs() / (1<<SI_LOAD_SHIFT); // out of 250

	data_t* data = &(buffer->data[header->data_wp]);
	data->uptime		= sys_info.uptime; // Need more than tens of years uptime?
	data->process		= sys_info.procs > (uint16_t)-1 ? (uint16_t)-1 : sys_info.procs;
	data->load1		= load1 > 255 ? 255 : load1;
	data->load5		= load5 > 255 ? 255 : load5;
	data->ram_free		= (uint64_t)(sys_info.freeram) * sys_info.mem_unit / DATA_UNITSIZE;
	data->ram_shared	= (uint64_t)(sys_info.sharedram) * sys_info.mem_unit / DATA_UNITSIZE;
	data->ram_buffer	= (uint64_t)(sys_info.bufferram) * sys_info.mem_unit / DATA_UNITSIZE;
	data->swap_free		= (uint64_t)(sys_info.freeswap) * sys_info.mem_unit / DATA_UNITSIZE;

	/*fprintf(stdout, "data_total %"PRIu32" Write pointer to %"PRIu32" %p %p.\n", header->data_total, header->data_wp, buffer, data);
	fprintf(stdout, "uptime %"PRIu32"\n", data->uptime);
	fprintf(stdout, "- process %"PRIu16"\n", data->process);
	fprintf(stdout, "- load %"PRIu8" %"PRIu8"\n", data->load1, data->load5);
	fprintf(stdout, "ram_total %"PRIu16"\n", header->ram_total);
	fprintf(stdout, "- ram_free %"PRIu16"\n", data->ram_free);
	fprintf(stdout, "- ram_shared %"PRIu16"\n", data->ram_shared);
	fprintf(stdout, "- ram_buffer %"PRIu16"\n", data->ram_buffer);
	fprintf(stdout, "swap_total %"PRIu16"\n", header->swap_total);
	fprintf(stdout, "- swap_free %"PRIu16"\n", data->swap_free);
	fprintf(stdout, "x %"PRIu16" x 4kB\n", header->data_blocksize);*/

	header->data_wp = header->data_wp == DATA_COUNT - 1 ? 0 : header->data_wp + 1;

	int pagesize = getpagesize();
	void* data_page = (void*)((uint64_t)data & ~(uint64_t)(pagesize - 1));
	if (data_page == header) {
		msync(data_page, pagesize, MS_ASYNC);
	}
	msync(header, pagesize, MS_SYNC);
	//fprintf(stdout, "%d B %p D %p %p\n", pagesize, buffer, data, data_page);
	
	munmap(buffer, sizeof(buffer_t));
	flock(file, LOCK_UN);
	close(file);
	return 0;
}