#define _GNU_SOURCE
#include "../android/host/files.c"
#include <assert.h>

void void_log(const char *format, ...) { (void)format; }
uint32_t void_get_last_error(void) { return 0; }
int void_obb_stat(const char *path, struct stat *out) { (void)path; (void)out; errno=ENOENT; return -1; }
int void_obb_list(const char *path, unsigned index, char *name, size_t capacity, int *directory, struct stat *st)
{ (void)path; (void)index; (void)name; (void)capacity; (void)directory; (void)st; return 1; }

static void watched_metadata(const char *path, int size)
{
    for (int i=0;i<200;++i) {
        struct stat st;
        int result=cached_stat(path,&st);
        if (size<0 && result<0 && errno==ENOENT) return;
        if (!result && st.st_size==size && !cached_stat_mode(path,&st,0) && st.st_size==size) return;
        usleep(1000); /* Shared-storage notifications can arrive after the writer returns. */
    }
    assert(!"configuration watch did not converge");
}

int main(int argc, char **argv)
{
    char root[1024];snprintf(root,sizeof root,"%s/bioshock-file-cache-XXXXXX",argc>1?argv[1]:"/data/local/tmp");
    assert(mkdtemp(root));
    snprintf(game_root,sizeof game_root,"%s",root);
    snprintf(data_root,sizeof data_root,"%s/userdata",root);
    char name[64];
    unsigned char kind;
    char absent[1024];snprintf(absent,sizeof absent,"%s/missing",root);
    uint64_t loads=listing_loads;
    for(int i=0;i<1000;++i)assert(listing_lookup(absent,"library.swf",name,sizeof name,&kind,1)==0);
    assert(listing_loads==loads+1);
    assert(mkdir(absent,0700)==0);
    char file[1100];snprintf(file,sizeof file,"%s/Library.swf",absent);
    FILE *f=fopen(file,"wb");assert(f);fputs("asset",f);fclose(f);
    listing_forget(absent);
    assert(listing_lookup(absent,"library.swf",name,sizeof name,&kind,1)==1&&!strcmp(name,"Library.swf"));
    char profile[1200];snprintf(profile,sizeof profile,"%s/Documents/Bioshock/SaveGames",data_root);
    assert(!cacheable(profile));
    snprintf(profile,sizeof profile,"%s/AppData/Roaming/FlashMovies/rus",data_root);assert(cacheable(profile));
    assert(!mkdir(data_root,0700));
    char documents[1200],game[1200],saves[1200],resolved[1400];
    snprintf(documents,sizeof documents,"%s/Documents",data_root);assert(!mkdir(documents,0700));
    snprintf(game,sizeof game,"%s/Bioshock",documents);assert(!mkdir(game,0700));
    snprintf(saves,sizeof saves,"%s/SaveGames",game);assert(!mkdir(saves,0700));
    int parent;unsigned char leaf_kind;
    loads=listing_loads;
    for(int i=0;i<1000;++i){
        assert(!resolve_below(resolved,sizeof resolved,data_root,"dOcUmEnTs\\bIoShOcK\\sAvEgAmEs\\Fresh.bsb",&parent,&leaf_kind,0));
        assert(parent);
    }
    assert(listing_loads==loads+3);
    char save[1400];snprintf(save,sizeof save,"%s/FRESH.BSB",saves);
    f=fopen(save,"wb");assert(f);fputs("save",f);fclose(f);
    assert(resolve_below(resolved,sizeof resolved,data_root,"documents\\bioshock\\savegames\\fresh.bsb",&parent,&leaf_kind,0));
    assert(parent&&!strcasecmp(resolved,save));
    struct stat st;assert(!cached_stat(resolved,&st)&&st.st_size==4);
    f=fopen(save,"ab");assert(f);fputs("changed",f);fclose(f);
    assert(!cached_stat(resolved,&st)&&st.st_size==11);
    assert(!unlink(save));
    assert(!resolve_below(resolved,sizeof resolved,data_root,"documents\\bioshock\\savegames\\fresh.bsb",&parent,&leaf_kind,0)&&parent);
    char config[1400];snprintf(config,sizeof config,"%s/User.ini",game);
    assert(cacheable(config)&&cached_stat_mode(config,&st,0)==-1);
    f=fopen(config,"wb");assert(f);fputs("ini",f);fclose(f);
    watched_metadata(config,3);
    assert(!cached_stat_mode(config,&st,0)&&st.st_size==3);
    f=fopen(config,"ab");assert(f);fputs("changed",f);fclose(f);
    watched_metadata(config,10);
    assert(!unlink(config));
    watched_metadata(config,-1);
    f=fopen(config,"wb");assert(f);fputs("new",f);fclose(f);
    watched_metadata(config,3);
    assert(resolve_below(resolved,sizeof resolved,data_root,"documents\\bioshock\\user.ini",&parent,&leaf_kind,0));
    assert(!strcasecmp(config,resolved));assert(!unlink(config));
    assert(!rmdir(saves)&&!rmdir(game)&&!rmdir(documents)&&!rmdir(data_root));
    char content[1200],movies[1200],movie[1400],expected[1400];
    snprintf(content,sizeof content,"%s/Content",root);assert(!mkdir(content,0700));
    snprintf(movies,sizeof movies,"%s/BinkMovies",content);assert(!mkdir(movies,0700));
    const char *codes[]={"int","deu","fra","esp","ita","rus"};
    for(unsigned i=0;i<sizeof codes/sizeof *codes;++i){
        snprintf(movie,sizeof movie,"%s/PlaneSequence%s%s.bik",movies,i?"_":"",i?codes[i]:"");
        f=fopen(movie,"wb");assert(f);fputc('A'+i,f);fclose(f);
    }
    mounts[0].win="C:\\Games\\BioShock";snprintf(mounts[0].host,sizeof mounts[0].host,"%s",root);mount_count=1;
    listing_forget(root);
    for(unsigned i=0;i<sizeof codes/sizeof *codes;++i){
        setenv("PORT_OPTION_GAME_LANGUAGE",codes[i],1);
        char *path=to_host_ex("C:\\Games\\BioShock\\Content\\BinkMovies\\PlaneSequence.bik",RESOLVE_OVERLAY,&parent,NULL,NULL);
        snprintf(expected,sizeof expected,"%s/PlaneSequence%s%s.bik",movies,i?"_":"",i?codes[i]:"");
        assert(path&&parent&&!strcasecmp(path,expected));free(path);
    }
    setenv("PORT_OPTION_GAME_LANGUAGE","../rus",1);assert(!strcmp(voice_language(),"int"));
    setenv("PORT_OPTION_GAME_LANGUAGE","rus",1);
    char mapped[1024];
    const char *map = "C:\\Games\\BioShock\\Content\\Maps\\0-Lighthouse";
    const char *bank = "C:\\Games\\BioShock\\Content\\Sounds_Windows\\streams_0_audio";
    char requested[1024], target[1024];
    setenv("PORT_OPTION_VOICE_LANGUAGE", "same", 1);
    assert(!strcmp(voice_language(), "rus"));
    setenv("PORT_OPTION_VOICE_LANGUAGE", "int", 1);
    snprintf(requested, sizeof requested, "%s_rus.bsm", map);
    snprintf(target, sizeof target, "%s_int.bsm", map);
    assert(!strcmp(voice_bank_path(requested, mapped), target));
    snprintf(requested, sizeof requested, "%s.rus_fsb", bank);
    snprintf(target, sizeof target, "%s.fsb", bank);
    assert(!strcmp(voice_bank_path(requested, mapped), target));
    char *english = to_host_ex("C:\\Games\\BioShock\\Content\\BinkMovies\\PlaneSequence.bik",
                              RESOLVE_OVERLAY, &parent, NULL, NULL);
    snprintf(expected, sizeof expected, "%s/PlaneSequence.bik", movies);
    assert(english && !strcasecmp(english, expected));
    free(english);
    setenv("PORT_OPTION_GAME_LANGUAGE", "int", 1);
    setenv("PORT_OPTION_VOICE_LANGUAGE", "rus", 1);
    snprintf(requested, sizeof requested, "%s_int.bsm", map);
    snprintf(target, sizeof target, "%s_rus.bsm", map);
    assert(!strcmp(voice_bank_path(requested, mapped), target));
    snprintf(requested, sizeof requested, "%s.fsb", bank);
    snprintf(target, sizeof target, "%s.rus_fsb", bank);
    assert(!strcmp(voice_bank_path(requested, mapped), target));
    snprintf(requested, sizeof requested, "%s.bsm", map);
    assert(voice_bank_path(requested, mapped) == requested);
    snprintf(requested, sizeof requested, "%s_music.fsb", bank);
    assert(voice_bank_path(requested, mapped) == requested);
    snprintf(requested, sizeof requested, "C:\\Games\\BioShock\\Content\\Localizedint.lbf");
    assert(voice_bank_path(requested, mapped) == requested);
    char *path=to_host_ex("C:\\Games\\BioShock\\Content\\BinkMovies\\PlaneSequence.bik",0,&parent,NULL,NULL);
    snprintf(expected,sizeof expected,"%s/PlaneSequence.bik",movies);assert(path&&!strcasecmp(path,expected));free(path);
    const char *plane = "C:\\Games\\BioShock\\Content\\BinkMovies\\PlaneSequence.bik";
    snprintf(movie, sizeof movie, "%s/PlaneSequence_rus.bkr", movies);
    f = fopen(movie, "wb"); assert(f); fputs("BKR1../Other.bik", f); fclose(f);
    assert(!bink_voice_alias(plane, mapped));
    f = fopen(movie, "wb"); assert(f); fputs("BKR1Other.bik", f); fclose(f);
    assert(bink_voice_alias(plane, mapped) && !strcmp(mapped, "C:\\Games\\BioShock\\Content\\BinkMovies\\Other.bik"));
    assert(!unlink(movie));

    const uint32_t bink = 0x1802F100, ids = 0x1802F440, selected = 0x1802F460;
    void *guest = GPTR(0x1802F000);
    assert(mmap(guest, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == guest);
    MEM32(0x1802F068) = 1;
    MEM32(bink + 0xD0) = 5;
    MEM32(bink + 0x250) = ids;
    MEM32(ids + 20) = 5;
    void_bioshock_bink_select_track(bink);
    assert(MEM32(0x1802F06C) == 0);
    MEM32(bink + 0xD0) = 6;
    void_bioshock_bink_select_track(bink);
    assert(MEM32(0x1802F06C) == 5);
    MEM32(bink + 0x2E8) = 1;
    MEM32(bink + 0x244) = selected;
    MEM32(selected) = 5;
    assert(void_bioshock_bink_volume_track(bink, 0) == 5);
    MEM32(selected) = 1;
    MEM32(0x1802F06C) = 1;
    setenv("PORT_OPTION_VOICE_LANGUAGE", "fra", 1);
    void_bioshock_bink_select_track(bink);
    assert(MEM32(0x1802F06C) == 1 && void_bioshock_bink_volume_track(bink, 1) == 1);
    assert(!munmap(guest, 4096));

    for(unsigned i=0;i<sizeof codes/sizeof *codes;++i){
        snprintf(movie,sizeof movie,"%s/PlaneSequence%s%s.bik",movies,i?"_":"",i?codes[i]:"");assert(!unlink(movie));
    }
    assert(!rmdir(movies)&&!rmdir(content));
    assert(!unlink(file)&&!rmdir(absent)&&!rmdir(root));
    puts("PASS production file cache, movie aliases and six-track Bink language selection");
    return 0;
}
