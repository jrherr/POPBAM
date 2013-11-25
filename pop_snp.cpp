/** \file pop_snp.cpp
 *  \brief Functions for extracting SNP calls from BAM files
 *  \author Daniel Garrigan
 *  \version 0.4
*/
#include "pop_snp.h"

int main_snp(int argc, char *argv[])
{
	bool found = false;           //! is the outgroup found?
	int i = 0;
	int k = 0;
	int chr = 0;                  //! chromosome identifier
	int beg = 0;                  //! beginning coordinate for analysis
	int end = 0;                  //! end coordinate for analysis
	int ref = 0;                  //! reference allele
	long num_windows = 0;         //! total number of windows
	long cw = 0;                  //! window counter
	std::string msg;              //! string for error message
	std::string region;           //! the scaffold/chromosome region string
	bam_plbuf_t *buf = nullptr;   //! pileup buffer
	snpData *t = nullptr;         //! data object for the snp function

	// allocate memory for nucdiv data structre
	t = new snpData;

	// parse the command line options
	region = t->parseCommandLine(argc, argv);

	// check input BAM file for errors
	t->checkBAM();

	// initialize the sample data structure
	t->bam_smpl_init();

	// add samples
	t->bam_smpl_add();

	// initialize error model
	t->em = errmod_init(0.17);

	// if outgroup option is used check to make sure it exists
	if (t->flag & BAM_OUTGROUP)
	{
		for (i=0; i < t->sm->n; i++)
		{
			if (strcmp(t->sm->smpl[i], t->outgroup.c_str()) == 0)
			{
				t->outidx = i;
				found = true;
			}
		}

		if (!found)
		{
			msg = "Specified outgroup " + t->outgroup + " not found";
			fatalError(msg);
		}
	}

	// parse genomic region
	k = bam_parse_region(t->h, region, &chr, &beg, &end);
	if (k < 0)
	{
		msg = "Bad genome coordinates: " + region;
		fatalError(msg);
	}

	// fetch reference sequence
	t->ref_base = faidx_fetch_seq(t->fai_file, t->h->target_name[chr], 0, 0x7fffffff, &(t->len));

	// calculate the number of windows
	if (t->flag & BAM_WINDOW)
		num_windows = ((end - beg) - 1) / t->win_size;
	else
	{
		t->win_size = end - beg;
		num_windows = 1;
	}

	// iterate through all windows along specified genomic region
	for (cw=0; cw < num_windows; cw++)
	{
		// construct genome coordinate string
		std::string scaffold_name(t->h->target_name[chr]);
		std::ostringstream winc(scaffold_name);
		winc.seekp(0, std::ios::end);
		winc << ":" << beg + (cw * t->win_size) + 1 << "-" << ((cw + 1) * t->win_size) + (beg - 1);
		std::string winCoord = winc.str();

		// initialize number of sites to zero
		t->num_sites = 0;

		// parse the BAM file and check if region is retrieved from the reference
		if (t->flag & BAM_WINDOW)
		{
			k = bam_parse_region(t->h, winCoord, &ref, &(t->beg), &(t->end));

			if (k < 0)
			{
				msg = "Bad window coordinates " + winCoord;
				fatalError(msg);
			}
		}
		else
		{
			ref = chr;
			t->beg = beg;
			t->end = end;

			if (ref < 0)
			{
				msg = "Bad scaffold name: " + region;
				fatalError(msg);
			}
		}

		// initialize diverge specific variables
		t->init_snp();

		// create population assignments
		t->assign_pops();

		// print ms header if first window iteration
		if ((t->output == 2) && (cw == 0))
			t->printMSHeader(num_windows);

		// initialize pileup
		buf = bam_plbuf_init(make_snp, t);

		// fetch region from bam file
		if ((bam_fetch(t->bam_in->x.bam, t->idx, ref, t->beg, t->end, buf, fetch_func)) < 0)
		{
			msg = "Failed to retrieve region " + region + " due to corrupted BAM index file";
			fatalError(msg);
		}

		// finalize pileup
		bam_plbuf_push(0, buf);

		// print results to stdout
		t->print_snp(chr);

		// take out the garbage
		t->destroy_snp();
		bam_plbuf_destroy(buf);
	}
	// end of window interation

	errmod_destroy(t->em);
	samclose(t->bam_in);
	bam_index_destroy(t->idx);
	t->bam_smpl_destroy();
	free(t->ref_base);
	delete t;

	return 0;
}

int make_snp(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data)
{
	int i = 0;
	int fq = 0;
	unsigned long long sample_cov = 0;
	unsigned long long *cb = nullptr;
	snpData *t = nullptr;

	// get control data structure
	t = (snpData*)data;

	// only consider sites located in designated region
	if ((t->beg <= (int)pos) && (t->end > (int)pos))
	{
		// call bases
		cb = callBase(t, n, pl);

		// resolve heterozygous sites
		if (!(t->flag & BAM_HETEROZYGOTE))
			cleanHeterozygotes(t->sm->n, cb, (int)t->ref_base[pos], t->min_snpQ);

		// determine if site is segregating
		fq = segBase(t->sm->n, cb, t->ref_base[pos], t->min_snpQ);

		// determine how many samples pass the quality filters
		sample_cov = qualFilter(t->sm->n, cb, t->min_rmsQ, t->min_depth, t->max_depth);
		
		unsigned int *ncov = nullptr;
		ncov = new unsigned int [t->sm->npops]();

		// determine population coverage
		for (i = 0; i < t->sm->npops; ++i)
		{
			unsigned long long pc = 0;

			pc = sample_cov & t->pop_mask[i];
			ncov[i] = bitcount64(pc);

			unsigned int req = (unsigned int)((t->min_pop * t->pop_nsmpl[i]) + 0.4999);

			if (ncov[i] >= req)
				t->pop_cov[t->num_sites] |= 0x1U << i;
		}	

		if (t->pop_cov[t->num_sites] > 0)
		{
			if (fq > 0)
			{
				// calculate the site type
				for (i = 0; i < t->sm->npops; ++i)
					t->ncov[i][t->segsites] = ncov[i];
				t->types[t->segsites] = calculateSiteType(t->sm->n, cb);

				// add to the haplotype matrix
				t->hap.pos[t->segsites] = pos;
				t->hap.ref[t->segsites] = bam_nt16_table[(int)t->ref_base[pos]];

				for (i = 0; i < t->sm->n; i++)
				{
					t->hap.rms[i][t->segsites] = (cb[i] >> (CHAR_BIT * 6)) & 0xffff;
					t->hap.snpq[i][t->segsites] = (cb[i] >> (CHAR_BIT * 4)) & 0xffff;
					t->hap.num_reads[i][t->segsites] = (cb[i]>>(CHAR_BIT * 2)) & 0xffff;
					t->hap.base[i][t->segsites] = bam_nt16_table[(int)iupac[(cb[i] >> CHAR_BIT) & 0xff]];

					if (cb[i] & 0x2ULL)
						t->hap.seq[i][t->segsites/64] |= 0x1ULL << t->segsites % 64;
				}
				t->hap.idx[t->segsites] = t->num_sites;
				t->segsites++;
			}
			t->num_sites++;
		}

		// take out the garbage
		delete [] cb;
		delete [] ncov;
	}

	return 0;
}

void snpData::print_snp(int chr)
{
	snp_func fp[3] = {&snpData::printSNP, &snpData::printSweep, &snpData::printMS};
	(this->*fp[output])(chr);
}

int snpData::printSNP(int chr)
{
	int i = 0;
	int j = 0;

	for (i = 0; i < segsites; i++)
	{
		std::string out = h->target_name[chr] + '\t' + hap.pos[i] + 1 + '\t';
		out += bam_nt16_rev_table[hap.ref[i]];

		for (j = 0; j < sm->n; j++)
		{
			out += '\t' + bam_nt16_rev_table[hap.base[j][i]];
			out += '\t' + hap.snpq[j][i];
			out += '\t' + hap.rms[j][i];
			out += '\t' + hap.num_reads[j][i];
		}

		std::cout << out << std::endl;
	}

	return 0;
}

int snpData::printSweep(int chr)
{
	int i = 0;
	int j = 0;
	unsigned short freq = 0;
	unsigned short pop_n = 0;
	unsigned long long pop_type = 0;

	for (i = 0; i < segsites; i++)
	{
		std::string out = h->target_name[chr] + '\t' + hap.pos[i] + 1;

		for (j = 0; j < sm->npops; j++)
		{
			// population-specific site type
			pop_type = types[j] & pop_mask[i];

			// polarize the mutation at the site
			if ((flag & BAM_OUTGROUP) && CHECK_BIT(types[i], outidx))
				freq = ncov[i][j] - bitcount64(pop_type);
			else
				freq = bitcount64(pop_type);

			out += '\t' + freq + '\t' + ncov[i][j];
		}

		std::cout << out << std::endl;
	}

	return 0;
}

int snpData::printMS(int chr)
{
	int i = 0;
	int j = 0;

	std::cout << "//" << std::endl;
	std::cout << "segsites: " << segsites << std::endl;
	std::cout << "positions: ";

	for (i = 0; i < segsites; i++)
		std::cout << std::setprecision(8) << (double)(hap.pos[i] - beg) / (end - beg) << " ";
	std::cout << std::endl;

	for (i = 0; i < sm->n; i++)
	{
		for (j = 0; j < segsites; j++)
		{
			if ((flag & BAM_OUTGROUP) && CHECK_BIT(types[hap.idx[j]], outidx))
			{
				if (CHECK_BIT(hap.seq[i][j/64], j % 64))
					std::cout << "0";
				else
					std::cout << "1";
			}
			else
			{
				if (CHECK_BIT(hap.seq[i][j/64], j % 64))
					std::cout << "1";
				else
					std::cout << "0";
			}
		}
		std::cout << std::endl;
	}
	std::cout << std::endl;
	return 0;
}

int snpData::printMSHeader(long nwindows)
{
	int i = 0;
	std::string out;

	if (sm->npops > 1)
	{
		out = "ms " + sm->n + ' ' + nwindows;
		out += " -t 5.0 -I ";
		out += sm->npops << ' ';

		for (i = 0; i < sm->npops; i++)
			out += (int)(pop_nsmpl[i]) + ' ';
	}
	else
	{
		out = "ms " + sm->n + ' ' + nwindows;
		out += " -t 5.0 ";
	}

	out += '\n';
	out += "1350154902";
	std::cout << out << std::endl << std::endl;
}

std::string snpData::parseCommandLine(int argc, char *argv[])
{
#ifdef _MSC_VER
	struct _stat finfo;
#else
	struct stat finfo;
#endif
	std::vector<std::string> glob_opts;
	std::string msg;

	GetOpt::GetOpt_pp args(argc, argv);

	args >> GetOpt::Option('f', reffile);
	args >> GetOpt::Option('h', headfile);
	args >> GetOpt::Option('m', min_depth);
	args >> GetOpt::Option('x', max_depth);
	args >> GetOpt::Option('q', min_rmsQ);
	args >> GetOpt::Option('s', min_snpQ);
	args >> GetOpt::Option('a', min_mapQ);
	args >> GetOpt::Option('b', min_baseQ);
	args >> GetOpt::Option('o', output);
	args >> GetOpt::Option('z', het_prior);
	args >> GetOpt::Option('p', outgroup);
	args >> GetOpt::Option('n', min_pop);
	args >> GetOpt::Option('w', win_size);

	if (args >> GetOpt::OptionPresent('w'))
	{
		win_size *= KB;
		flag |= BAM_WINDOW;
	}
	if (args >> GetOpt::OptionPresent('h'))
		flag |= BAM_HEADERIN;
	if (args >> GetOpt::OptionPresent('v'))
		flag |= BAM_VARIANT;
	if (args >> GetOpt::OptionPresent('i'))
		flag |= BAM_ILLUMINA;
	if (args >> GetOpt::OptionPresent('z'))
		flag |= BAM_HETEROZYGOTE;
	if (args >> GetOpt::OptionPresent('p'))
		flag |= BAM_OUTGROUP;

	args >> GetOpt::GlobalOption(glob_opts);

	// run some checks on the command line

	// check if output option is valid
	if ((output < 0) || (output > 2))
		printUsage("Not a valid output option");

	// if no input BAM file is specified -- print usage and exit
	if (glob_opts.size() < 2)
		printUsage("Need to specify input BAM file name");
	else
		bamfile = glob_opts[0];

	// check if specified BAM file exists on disk
	if ((stat(bamfile.c_str(), &finfo)) != 0)
	{
		msg = "Specified input file: " + bamfile + " does not exist";

		switch(errno)
		{
		case ENOENT:
			std::cerr << "File not found" << std::endl;
			break;
		case EINVAL:
			std::cerr << "Invalid parameter to stat" << std::endl;
			break;
		default:
			std::cerr << "Unexpected error in stat" << std::endl;
			break;
		}

		fatalError(msg);
	}

	// check if fastA reference file is specified
	if (reffile.empty())
		printUsage("Need to specify a fasta reference file");

	// check is fastA reference file exists on disk
	if ((stat(reffile.c_str(), &finfo)) != 0)
	{
		switch(errno)
		{
		case ENOENT:
			std::cerr << "File not found" << std::endl;
			break;
		case EINVAL:
			std::cerr << "Invalid parameter to stat" << std::endl;
			break;
		default:
			std::cerr << "Unexpected error in stat" << std::endl;
			break;
		}

		msg = "Specified reference file: " + reffile + " does not exist";
		fatalError(msg);
	}

	//check if BAM header input file exists on disk
	if (flag & BAM_HEADERIN)
	{
		if ((stat(headfile.c_str(), &finfo)) != 0)
		{
			switch(errno)
			{
			case ENOENT:
				std::cerr << "File not found" << std::endl;
				break;
			case EINVAL:
				std::cerr << "Invalid parameter to stat" << std::endl;
				break;
			default:
				std::cerr << "Unexpected error in stat" << std::endl;
				break;
			}

			msg = "Specified header file: " + headfile + " does not exist";
			fatalError(msg);
		}
	}

	// return the index of first non-optioned argument
	return glob_opts[1];
}

snpData::snpData(void)
{
	derived_type = SNP;
	output = 0;
	outidx = 0;
	win_size = 0;
	min_pop = 1.0;
}

void snpData::init_snp(void)
{
	int i = 0;
	const int length = end - beg;
	const int n = sm->n;
	const int npops = sm->npops;

	segsites = 0;

	try
	{
		types = new unsigned long long [length]();
		pop_mask = new unsigned long long [npops]();
		pop_nsmpl = new unsigned char [npops]();
		pop_cov = new unsigned int [length]();
		hap.pos = new unsigned int [length]();
		hap.idx = new unsigned int [length]();
		hap.ref = new unsigned char [length]();
		hap.seq = new unsigned long long* [n];
		hap.base = new unsigned char* [n];
		hap.rms = new unsigned short* [n];
		hap.snpq = new unsigned short* [n];
		hap.num_reads = new unsigned short* [n];
		ncov = new unsigned int* [npops];

		for (i=0; i < n; i++)
		{
			hap.seq[i] = new unsigned long long [length]();
			hap.base[i] = new unsigned char [length]();
			hap.rms[i] = new unsigned short [length]();
			hap.snpq[i] = new unsigned short [length]();
			hap.num_reads[i] = new unsigned short [length]();
		}

		for (i=0; i < npops; i++)
			ncov[i] = new unsigned int [length]();
	}
	catch (std::bad_alloc& ba)
	{
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
	}
}

void snpData::destroy_snp(void)
{
	int i = 0;
	int npops = sm->npops;

	delete [] pop_mask;
	delete [] pop_nsmpl;
	delete [] pop_cov;
	delete [] types;
	delete [] hap.pos;
	delete [] hap.idx;
	delete [] hap.ref;
	for (i=0; i < sm->n; i++)
	{
		delete [] hap.seq[i];
		delete [] hap.base[i];
		delete [] hap.num_reads[i];
		delete [] hap.snpq[i];
		delete [] hap.rms[i];
	}
	for (i = 0; i < npops; i++)
		delete [] ncov[i];
	delete [] ncov;
	delete [] hap.seq;
	delete [] hap.base;
	delete [] hap.snpq;
	delete [] hap.rms;
	delete [] hap.num_reads;
}

void snpData::printUsage(std::string msg)
{
	std::cerr << msg << std::endl << std::endl;
	std::cerr << "Usage:   popbam snp [options] <in.bam> [region]" << std::endl << std::endl;
	std::cerr << "Options: -i          base qualities are Illumina 1.3+               [ default: Sanger ]" << std::endl;
	std::cerr << "         -h  FILE    Input header file                              [ default: none ]" << std::endl;
	std::cerr << "         -v          output variant sites only                      [ default: all sites ]" << std::endl;
	std::cerr << "         -z  FLT     output heterozygous base calls                 [ default: consensus ]" << std::endl;
	std::cerr << "         -w  INT     use sliding window of size (kb)" << std::endl;
	std::cerr << "         -p  STR     sample name of outgroup                        [ default: reference ]" << std::endl;
	std::cerr << "         -o  INT     output format                                  [ default: 0 ]" << std::endl;
	std::cerr << "                     0 : popbam snp format" << std::endl;
	std::cerr << "                     1 : SweepFinder snp format" << std::endl;
	std::cerr << "                     2 : MS format" << std::endl;
	std::cerr << "         -n  FLT     minimum proportion of population covered       [ default: 1.0 ]" << std::endl;
	std::cerr << "         -f  FILE    Reference fastA file" << std::endl;
	std::cerr << "         -m  INT     minimum read coverage                          [ default: 3 ]" << std::endl;
	std::cerr << "         -x  INT     maximum read coverage                          [ default: 255 ]" << std::endl;
	std::cerr << "         -q  INT     minimum rms mapping quality                    [ default: 25 ]" << std::endl;
	std::cerr << "         -s  INT     minimum snp quality                            [ default: 25 ]" << std::endl;
	std::cerr << "         -a  INT     minimum map quality                            [ default: 13 ]" << std::endl;
	std::cerr << "         -b  INT     minimum base quality                           [ default: 13 ]" << std::endl << std::endl;
	exit(EXIT_FAILURE);
}
