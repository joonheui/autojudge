#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/time.h> 
#include <signal.h>

#define BUFF_SIZE 2000
#define MAX_FILES 20 

char *file_list[MAX_FILES];  
char *input_file = NULL;
char *output_file = NULL;
long time_limit = 0;
char *target = NULL;
char *target_exe = NULL;
int file_count = 0;

struct timeval start, end; 
long microseconds; 
long running_time = 0;

int correct_cnt = 0;
int wrong_cnt = 0;
int timeout_cnt = 0;

extern int optind, opterr, optopt;
extern char *optarg;

static void usageError(char *progName, char *msg, int opt) {
    fprintf(stderr, "Usage: %s [-i inputdir] [-a outputdir] [-t timelimit] [target]\n", progName);
    exit(EXIT_FAILURE);
}

static void *checkDir(char *dirPath) {
    DIR *dirp;
    dirp = opendir(dirPath);

    if(dirp == NULL) {
        fprintf(stderr, "Failed to open directory <%s>\n", dirPath);
        exit(1);
    }
    closedir(dirp);
    
    return dirPath;
}

static void *checkTarget(char *target) {
    size_t target_len = strlen(target);

    if (target_len < 2) {
        fprintf(stderr, "Invalid file name <%s>\n", target);
        exit(1);
    }
    
    if (target[target_len-2] == '.' && target[target_len-1] == 'c') {
        return target;
    } else {
        fprintf(stderr, "Wrong file name <%s>\n", target);
        exit(1);
    }
}

char *getTargetName(char *target) {
    size_t len = strlen(target);

    char *target_exe = (char *)malloc(len - 1);     
    if (!target_exe) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }

    strncpy(target_exe, target, len - 2);
    target_exe[len - 2] = '\0';  

    return target_exe;
}

int checkTimelimit(char *str) {
    while (*str) {
        if (!isdigit((unsigned char)*str)) {
            return 0;
        }
        str++;
    }

    return 1;
}

void parseCLI(int argc, char *argv[]) {
    int opt;
    
    if (argc != 8) {
        usageError(argv[0], "Wrong command-line interface", optopt);
    }

    while ((opt = getopt(argc, argv, ":i:a:t:")) != -1) {
        switch (opt) {
            case 'i':
                input_file = checkDir(optarg);
                break;
            case 'a':
                output_file = checkDir(optarg);
                break;
            case 't':
                if (checkTimelimit(optarg)){ 
                    time_limit = atoi(optarg); 
                } else {
                    usageError(argv[0], "Wrong type (timelimit)", optopt);
                }
                break;
            case ':':
                usageError(argv[0], "Missing argument", optopt);
                break;
            case '?':
                usageError(argv[0], "Unrecognized option", optopt);
                break;
        }
    }

    if (optind < argc) { 
        target = checkTarget(argv[optind]); 
        target_exe = getTargetName(argv[optind]); 
    }
}

void compileProgram(char *target_exe, char *target) {
    int fd[2];
    if (pipe(fd) == -1) {
        perror("Pipe error");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFF_SIZE];
    ssize_t count;
    pid_t childPid1 = fork();

    if (childPid1 == -1) {
        perror("Child 1 fork error");
        exit(EXIT_FAILURE);
    }

    if (childPid1 == 0) { 
        close(fd[0]);
        dup2(fd[1], STDERR_FILENO); 
        close(fd[1]);

        execl("/usr/bin/gcc", "gcc", "-o", target_exe, target, (char *) NULL);
        
        perror("Gcc Error\n");
        exit(1);
    } else { 
        close(fd[1]); 

        wait(0);
        
        count = read(fd[0], buffer, sizeof(buffer) - 1);

        close(fd[0]);

        if (count > 0) {
            buffer[count] = '\0';
            printf("Compile Error\n");
        } 
    }    
}

pid_t sourcePid;
void handle_alarm(int sig) {
    if (kill(sourcePid, 0) == 0) { 
        kill(sourcePid, SIGKILL);
    }
}

void score_output(char *output, char *answer, long micro) {
    if (strcmp(output, answer) == 0) { 
        printf("CORRECT\n");
        printf("Execution time : %ld microseconds\n", micro);
        correct_cnt += 1;
        running_time += micro;
    } else {
        printf("WRONG\n");
        wrong_cnt += 1;
    }
}

void timeout() {
    printf("Timeout\n");
    timeout_cnt += 1;
}

void runtime_error() {
    printf("Runtime Error\n");
}

void set_sigaction() {
    struct sigaction sa;
    sa.sa_handler = handle_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void set_itimer() {
    struct itimerval timer; 

    timer.it_value.tv_sec = time_limit / 1000000;
    timer.it_value.tv_usec = time_limit % 1000000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;

    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        perror("setitimer");
        exit(EXIT_FAILURE);
    }
}

int check_child_status(int status) {
    if (WIFEXITED(status)) {
        return 1;
    } else if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == 9) {
            return 2;
        } else {
            return 3;
        }
    } else if (WIFSTOPPED(status)) {
        printf("Child process stopped by signal %d (%s)\n", WSTOPSIG(status), strsignal(WSTOPSIG(status)));
        return 0;
    } else if (WIFCONTINUED(status)) {
        printf("Child process continued\n");
        return 0;
    } 
    else {
        printf("Child process ended unexpectedly\n");
        return 0;
    }
}

static void runSrc(char *target_exe, char *line1, char *line2) {
    char *prog_name;
    asprintf(&prog_name, "./%s", target_exe); 

    int fd[2];
    int fd2[2];
    if (pipe(fd) == -1 || pipe(fd2) == -1) {
        perror("pipe failed");
        exit(EXIT_FAILURE);
    }

    char targetOutput[BUFF_SIZE];
    sourcePid = fork();

    if (sourcePid == 0) {
        close(fd[1]); 
        dup2(fd[0], STDIN_FILENO); 
        close(fd[0]); 

        close(fd2[0]); 
        dup2(fd2[1], STDOUT_FILENO); 
        close(fd2[1]); 

        execl(prog_name, target_exe, (char *)NULL);
        
        perror("execl failed");
        exit(EXIT_FAILURE);
    } else {  
        set_sigaction();    
        set_itimer(); 

        close(fd[0]); 
        close(fd2[1]); 

        gettimeofday(&start, NULL);

        write(fd[1], line1, strlen(line1)); 
        close(fd[1]);

        ssize_t outputCount;
        outputCount = read(fd2[0], targetOutput, BUFF_SIZE - 1);
        if (outputCount > 0) {
            targetOutput[outputCount] = '\0';

            if (targetOutput[outputCount - 1] == '\n') {
                targetOutput[outputCount - 1] = '\0';
            }
        }

        close(fd2[0]);

        int status;
        waitpid(sourcePid, &status, 0);

        gettimeofday(&end, NULL);
        microseconds = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);

        int code = check_child_status(status);
        switch(code) {
            case 1:
                score_output(targetOutput, line2, microseconds);
                break;
            case 2:
                timeout();
                break;
            case 3:
                runtime_error();
                break;
            default:
                break;
        }
    }
}

int compare(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

int sorting_filename() {
    DIR *input_dirp;
    struct dirent *input_dp;

    int file_count = 0;

    input_dirp = opendir(input_file);
    if(input_dirp == NULL) {
        fprintf(stderr, "Failed to open directory <%s>\n", input_file);
        exit(1);
    }

    while ((input_dp = readdir(input_dirp)) != NULL) {
        if (input_dp->d_type == DT_REG) {
            file_list[file_count] = strdup(input_dp->d_name);
            file_count++;
            if (file_count >= MAX_FILES) {
                fprintf(stderr, "Input files exceed 20.\n");
                break;
            }
        }
    }
    closedir(input_dirp);

    qsort(file_list, file_count, sizeof(char *), compare);

    return file_count;
}

void executeProgram(){
    file_count = sorting_filename();

    for (int i = 0; i < file_count; i++) {
        char input_file_path[BUFF_SIZE];
        snprintf(input_file_path, sizeof(input_file_path), "%s/%s", input_file, file_list[i]);

        char output_file_path[BUFF_SIZE];
        snprintf(output_file_path, sizeof(output_file_path), "%s/%s", output_file, file_list[i]);

        FILE *file1 = fopen(input_file_path, "r");
        FILE *file2 = fopen(output_file_path, "r");
        if (file1 == NULL || file2 == NULL) {
            perror("Cannot open the file\n");
            exit(EXIT_FAILURE);
        }

        printf("=== %s ===\n", input_file_path);


        fseek(file1, 0, SEEK_END);
        long file1_size = ftell(file1);
        fseek(file1, 0, SEEK_SET);

        fseek(file2, 0, SEEK_END);
        long file2_size = ftell(file2);
        fseek(file2, 0, SEEK_SET);

        char *line1 = (char *)malloc(file1_size + 1);
        char *line2 = (char *)malloc(file2_size + 1); 
        if (line1 == NULL || line2 == NULL) {
            perror("memory allocation failed");
            fclose(file1);
            fclose(file2);
            continue;
        }

        fread(line1, 1, file1_size, file1);
        fread(line2, 1, file2_size, file2);

        runSrc(target_exe, line1, line2);

        free(line1);
        free(line2);
        fclose(file1);
        fclose(file2);
        free(file_list[i]);

        printf("====================\n\n");
    }
}

void printJudgement() {
    if (file_count == correct_cnt) {
        printf("All tests passed!\n");
        printf("Running Time: %ld\n", running_time);
    } else {
        printf("Correct: %d, Wrong: %d, Timeout: %d\n", correct_cnt, wrong_cnt, timeout_cnt);
    }
}

int main(int argc, char *argv[])
{
    parseCLI(argc, argv);

    compileProgram(target_exe, target);
    
    executeProgram();

    printJudgement();

    exit(EXIT_SUCCESS);
}