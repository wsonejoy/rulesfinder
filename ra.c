#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <avl.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LINELEN         200
#define MAXMEM          28000000000L

unsigned int matchlimit;
unsigned int nbthreads;
unsigned int nbfiles;
avl_tree_t * dictroot;

// bloom filter params
#define BLOOM_WIDTH	28LL
#define BLOOM_TYPE	unsigned long long
#define BLOOM_SIZE	((BLOOM_TYPE) 1<<BLOOM_WIDTH)
#define BLOOM_STORAGE	((BLOOM_SIZE / 8)+1)
#define BLOOM_MASK	(BLOOM_SIZE-1)

inline void SETBIT(BLOOM_TYPE * ptr, unsigned long hash)
{
	unsigned long index;
	unsigned long dec;

	hash &= BLOOM_MASK;
	index = hash / (sizeof(BLOOM_TYPE)*8);
	dec = hash & (sizeof(BLOOM_TYPE)*8-1);
	ptr[index] |= 1 << dec;
}


inline unsigned int GETBIT(BLOOM_TYPE * ptr, unsigned long hash)
{
	unsigned long index;
	unsigned long dec;

	hash &= BLOOM_MASK;
	index = hash / (sizeof(BLOOM_TYPE)*8);
	dec = hash & (sizeof(BLOOM_TYPE)*8-1);
	return ptr[index]&(1<<dec);
}

struct s_rule
{
	char * rule;
	avl_tree_t * coverage;
};

struct s_rule_link
{
	char * rule;
	avl_tree_t * coverage;
	struct s_rule_link * next;
};

struct s_rulejob
{
	char * filename;
	struct s_rule_link * root;
	struct s_rule_link * tail;
	int done_by;
} * rulejob;

pthread_mutex_t rj_mutex = PTHREAD_MUTEX_INITIALIZER;

struct s_rulefile
{
	char * name;
	unsigned int nblines;
	unsigned int nbrules;
	avl_tree_t * ruleroot;
};

struct s_dictentry
{
	char * word;
	unsigned long hash;
	unsigned int index;
};

#define HASH_INIT 14695981039346656037ULL
#define HASH_STEP(hash,nchar) hash = (hash*1099511628211ULL)^((unsigned char) (nchar))
unsigned long hash(char * str)
{
        unsigned long hash = HASH_INIT;
        unsigned int i;

        for(i = 0; str[i] ; i++)
        {
                HASH_STEP(hash,str[i]);
        }
        return hash;
}

void usage(void)
{
	printf("usage: rulenalyzer cutout nbthreads [rule files ...]\n");
	exit(0);
}

void rule_free(struct s_rule * rule)
{
	if(rule->coverage)
		avl_free_tree(rule->coverage);
	if(rule->rule)
		free(rule->rule);
	free(rule);
}

int rule_compare(struct s_rule * a, struct s_rule * b)
{
	return strcmp(a->rule,b->rule);
}

void * xmalloc(unsigned int size)
{
	void * out;
	out = malloc(size);
	if(out)
		return out;
	perror("malloc");
	exit(5);
}

int int_cmp(unsigned int * a, unsigned int * b)
{
	if(*a>*b)
		return 1;
	if(*b>*a)
		return -1;
	return 0;
}

int ptr_cmp(unsigned int * a, unsigned int * b)
{
	if(a>b)
		return 1;
	if(b>a)
		return -1;
	return 0;
}

struct s_rule_link * newlink(char * rule)
{
	struct s_rule_link * out;

	out = xmalloc(sizeof(struct s_rule_link));
	out->rule = strdup(rule);
	if(out->rule == NULL)
	{
		perror("strdup");
		exit(50);
	}
	//out->coverage = avl_alloc_tree((avl_compare_t)int_cmp, (avl_freeitem_t)free);
	out->coverage = avl_alloc_tree((avl_compare_t)ptr_cmp, NULL);
	return out;
}

void setlimits(void)
{
        struct rlimit rlim;

	rlim.rlim_cur = MAXMEM;
        rlim.rlim_max = MAXMEM;
        if(setrlimit(RLIMIT_AS, &rlim))
        {
                perror("setrlimit");
                exit(3);
        }
}

int dict_compare(struct s_dictentry * a, struct s_dictentry * b)
{
	if(a->hash>b->hash)
		return 1;
	if(a->hash<b->hash)
		return -1;
	return strcmp(a->word, b->word);
}

void dict_free(struct s_dictentry * a)
{
	free(a->word);
	free(a);
}

void * load_rules(unsigned int * tid)
{
	struct s_rule_link * root;
	int rulefd;
	char rulestr[LINELEN];
	unsigned int curval;
	unsigned int nbpwds;
	//unsigned int * pcurval;
	unsigned int i;
	struct s_rule_link * link;
    unsigned int nbloaded;

	unsigned int jobid;

	while(1)
	{
		root = NULL;
		/* select jobid */
		pthread_mutex_lock( &rj_mutex );
		for(jobid=0;jobid<nbfiles;jobid++)
			if( rulejob[jobid].done_by == -1 )
				break;
		if(jobid>=nbfiles)
		{
			pthread_mutex_unlock( &rj_mutex );
			break;
		}
		rulejob[jobid].done_by = *tid;
		pthread_mutex_unlock( &rj_mutex );

		rulefd = open(rulejob[jobid].filename, O_RDONLY);
		if(rulefd <0)
		{
			perror(rulejob[jobid].filename);
			continue;
		}


        nbloaded = 0;
		while(1)
		{
			if(read(rulefd, &curval, sizeof(unsigned int)) != sizeof(unsigned int))
				break;
			if(curval>=(LINELEN-1))
			{
				fprintf(stderr, "rule length too large : %d>=%d\n", curval, LINELEN-1);
				break;
			}
			if(read(rulefd, rulestr, curval) != curval)
			{
				fprintf(stderr, "could not read rule string of len %d", curval);
				break;
			}
			rulestr[curval]=0;
			if(read(rulefd, &nbpwds, sizeof(unsigned int)) != sizeof(unsigned int))
			{
				fprintf(stderr, "could not read nbpwd\n");
				break;
			}
            if(nbpwds >= matchlimit)
            {
                nbloaded++;
                link = newlink(rulestr);
                for(i=0;i<nbpwds;i++)
                {
                    //pcurval = xmalloc(sizeof(unsigned int));
                    //if( read(rulefd, pcurval, sizeof(unsigned int)) != sizeof(unsigned int) )
                    if( read(rulefd, &curval, sizeof(unsigned int)) != sizeof(unsigned int) )
                    {
                        fprintf(stderr, "could not read password %d/%d\n", i, nbpwds);
                        break;
                    }
                    if(avl_insert(link->coverage, curval) == NULL)
                    {
                        perror("avl_insert");
                        exit(1);
                    }
                }
                link->next = rulejob[jobid].root;
                if(rulejob[jobid].tail == NULL)
                    rulejob[jobid].tail = link;
                rulejob[jobid].root = link;
            }
            else
            {
                lseek(rulefd, sizeof(unsigned int)*nbpwds, SEEK_CUR);
            }
		}

		close(rulefd);

		fprintf(stderr, "%s loaded [%d]\n", rulejob[jobid].filename, nbloaded);

	}
	fprintf(stderr, "[%d] EXITS\n", *tid);
	return NULL;
}

int main(int argc, char ** argv)
{
	unsigned int i;
	pthread_t * threads;
	unsigned int * ids;

	unsigned int maxval;
	unsigned int curval;
	struct s_rule_link * root;
	struct s_rule_link * prevlink;
	struct s_rule_link * curlink;
	struct s_rule_link * tmplink;
	struct s_rule_link * maxlink;
	avl_tree_t * oldcoverage;
	avl_node_t * scoverage;
	avl_node_t * scoverage2;
	BLOOM_TYPE * bloom;
//	unsigned int * pcurindex;
	unsigned int curindex;
	unsigned int nbleft;

	setlimits();
	if(argc<4)
		usage();
	matchlimit = atoi(argv[1]);
	if(matchlimit == 0)
		usage();
	nbthreads = atoi(argv[2]);
	if(nbthreads == 0)
		nbthreads = 1;
	
	nbfiles = argc-3;
	threads = xmalloc(sizeof(pthread_t)*nbthreads);
	memset(threads, 0, sizeof(pthread_t)*nbthreads);
	ids = xmalloc(sizeof(unsigned int)*nbthreads);
	rulejob = xmalloc(sizeof(struct s_rulejob)*nbfiles);
	bloom = xmalloc(BLOOM_STORAGE);
	for(i=0;i<nbfiles;i++)
	{
		rulejob[i].root = NULL;
		rulejob[i].tail = NULL;
		rulejob[i].done_by = -1;
		rulejob[i].filename = argv[i+3];
	}

	for(i=0;i<nbthreads;i++)
	{
		ids[i] = i;
		if(pthread_create(&threads[i], NULL, load_rules, &ids[i]))
		{
			fprintf(stderr, "error, could not create thread for file %s\n", argv[i+4]);
			perror("pthread_create");
			return 1;
		}
	}

	for(i=0;i<nbthreads;i++)
		pthread_join( threads[i], NULL );

	root = NULL;
	for(i=0;i<nbfiles;i++)
	{
		if(rulejob[i].tail == NULL)
			continue;
		rulejob[i].tail->next = root;
		root = rulejob[i].root;
	}
	free(rulejob);

	fprintf(stderr, "start crunching\n");

	oldcoverage = NULL;
	while(1)
	{
		maxval = 0;
		curlink = root;
		prevlink = NULL;
		maxlink = NULL;
		nbleft = 0;
		while(curlink)
		{
			if(curlink->coverage == NULL)
				curval = 0;
			else
			{
				if(oldcoverage)
				{
					/* coverage cleanup */
					scoverage = curlink->coverage->head;
					while(scoverage)
					{
						//pcurindex = scoverage->item;
						//curindex = *pcurindex;
						curindex = scoverage->item;
						//if(GETBIT(bloom, curindex) && avl_search(oldcoverage, pcurindex))
						if(GETBIT(bloom, curindex) && avl_search(oldcoverage, curindex))
						{
							scoverage2 = scoverage->next;
							avl_delete_node(curlink->coverage, scoverage);
							scoverage = scoverage2;
						}
						else
						{
							nbleft++;
							scoverage = scoverage->next;
						}
					}
				}
				curval = avl_count(curlink->coverage);
			}
			if(curval<matchlimit)
			{
				if(curlink->rule)
				{
					free(curlink->rule);
				}
				if(curlink->coverage)
				{
					avl_free_tree(curlink->coverage);
				}
				if(prevlink)
					prevlink->next = curlink->next;
				else
					root = curlink->next;
				tmplink = curlink;
				curlink = curlink->next;
				free(tmplink);
				continue;
			}
			if(curval>maxval)
			{
				maxval = curval;
				maxlink = curlink;
			}
			prevlink = curlink;
			curlink = curlink->next;
		}
		if(maxlink == NULL)
			break;
		if(oldcoverage)
			avl_free_tree(oldcoverage);
		oldcoverage = maxlink->coverage;

		/* build bloom filter */
		memset(bloom, 0, BLOOM_STORAGE);
		scoverage = oldcoverage->head;
		while(scoverage)
		{
			//pcurindex = scoverage->item;
			//curindex = *pcurindex;
			curindex = scoverage->item;
			SETBIT(bloom, curindex);
			scoverage = scoverage->next;
		}

		maxlink->coverage = NULL;
		printf("%s NBPWD=%d\n", maxlink->rule, maxval);
		fprintf(stderr, "%s NBPWD=%d NBRULES=%d\n", maxlink->rule, maxval, nbleft);
	}
	return 0;
}
