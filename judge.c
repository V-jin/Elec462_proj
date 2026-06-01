/*
 * gcc -o judge teampj.c -lpthread -lseccomp
 * 디렉토리 구조: 실행 파일이 들어있는 디렉토리 내부에 문제 번호가 이름으로 되어 있는 디렉토리들이 존재함
 * 각 문제 디렉토리 안에는 Generators(입력 생성기), Correct(정답 코드), Testcase(고정 입력값) 폴더가 있음
 * 입력 생성기와 정답 코드는 컴파일된 실행 파일, 고정 입력값은 텍스트 파일 형식이어야 함
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <termios.h>
#include <seccomp.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

// 시스템 설정 및 타임아웃 매크로
#define MAX_BUFFER          4096
#define TEST_LIMIT          50
#define TIME_LIMIT_MS       2000
#define MEM_LIMIT_KB        (256 * 1024)
#define POLL_TIMEOUT        (TIME_LIMIT_MS + 500)

// UI 출력용 텍스트 매크로
#define MSG_TITLE           "=========================================\n           오프라인 저지 설정\n=========================================\n\n"
#define MSG_PROMPT_LANG     "사용할 언어를 방향키(위/아래)로 선택하고 Enter를 누르시오:\n\n"
#define MSG_INPUT_PROB      "채점할 문제의 번호(종료하려면 'q' 입력): "
#define MSG_INPUT_PATH      "채점할 파일의 상대 경로: "
#define MSG_SETTING_DONE    "설정 완료\n"
#define MSG_COMPILE_ING     "\n소스 코드 컴파일 중\n"
#define MSG_COMPILE_ERR     "컴파일 에러 발생\n"
#define MSG_JUDGE_START     "\n채점을 시작함 (중단: Ctrl+C)\n"
#define MSG_JUDGE_END       "\n반례 탐색을 종료하려면 Enter 또는 q를 누르시오"
#define MSG_JUDGE_ABORT     "\n채점이 중단되었음\n"
#define MSG_SIGINT_ABORT    "\n채점이 중단되었음 (SIGINT)\n"
#define MSG_GOODBYE         "\n프로그램을 종료함\n"
#define MSG_ALL_PASS        "\n%d개의 테스트 케이스를 모두 통과함\n"
#define MSG_INPUT_TC        "반례 탐색 시도 횟수(5~3000): "

// 채점 결과 문자열 매크로
#define STR_WA              "오답"
#define STR_TLE             "시간 초과"
#define STR_MLE             "메모리 초과"
#define STR_RE              "런타임 에러"

// 컴파일 및 실행 명령어 포맷
#define CMD_FMT_GCC         "gcc %s -o %s"
#define CMD_FMT_GPP         "g++ %s -o %s"
#define CMD_FMT_PYTHON      "python3 %s"
#define BIN_TEMP_NAME       "temp_bin"

volatile pid_t gen_pid = -1; // 제너레이터(입력 생성기) 프로세스의 PID
volatile pid_t sol_pid = -1; // 정답 코드 프로세스의 PID
volatile pid_t sub_pid = -1; // 제출(채점 대상) 코드 프로세스의 PID
volatile sig_atomic_t interrupted = 0; // SIGINT(Ctrl+C) 수신 여부를 기록하는 상태 플래그

struct termios orig_termios; // 프로그램 시작 시점의 원본 터미널 설정을 백업하는 구조체

typedef enum {
    STATUS_WA,  // 오답 (Wrong Answer)
    STATUS_TLE, // 시간 초과 (Time Limit Exceeded)
    STATUS_MLE, // 메모리 초과 (Memory Limit Exceeded)
    STATUS_RE   // 런타임 에러 (Runtime Error)
} ErrorType;

typedef struct FailNode {
    int test_number; // 실패한 테스트 케이스의 번호
    ErrorType error_type; // 발생한 오류의 종류 (WA, TLE, MLE, RE)
    char input[MAX_BUFFER]; // 제너레이터 또는 파일로부터 주입된 입력 데이터
    char expected[MAX_BUFFER]; // 정답 코드가 출력한 예상 결과
    char actual[MAX_BUFFER]; // 제출 코드가 출력한 실제 결과
    struct FailNode *prev; // 이중 연결 리스트의 이전 노드 포인터
    struct FailNode *next; // 이중 연결 리스트의 다음 노드 포인터
} FailNode;

/*
 * 치명적인 시스템 콜 오류 발생 시 호출됨
 * perror를 통해 에러 내용을 표준 에러로 출력하며 즉시 프로세스를 종료함
 */
void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/*
 * 채점 도중 SIGINT(Ctrl+C) 수신 시 실행되는 시그널 핸들러
 * 실행 중인 백그라운드 자식 프로세스들을 SIGKILL로 강제 종료하며 메인 흐름으로 복귀할 수 있도록 플래그를 설정함
 */
void sigint_handler(int sig) {
    if (gen_pid > 0) kill(gen_pid, SIGKILL);
    if (sol_pid > 0) kill(sol_pid, SIGKILL);
    if (sub_pid > 0) kill(sub_pid, SIGKILL);
    
    interrupted = 1;
    write(STDOUT_FILENO, MSG_SIGINT_ABORT, sizeof(MSG_SIGINT_ABORT) - 1);
}

/*
 * SIGINT 시그널을 사용자 정의 핸들러와 매핑함
 * 시스템 콜 도중 끊김을 방지하기 위해 SA_RESTART 플래그를 적용함
 */
void setup_signals() {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1)
        die("sigaction error");
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal SIGPIPE error");
}

/*
 * seccomp를 초기화하여 허용 목록 기반으로 시스템 콜 규칙을 생성함
 * 네트워크 통신(socket, connect 등)을 블랙리스트 방식으로 명시적 차단하며 샌드박스 내부의 악의적 외부 유출을 통제함
 */
void setup_seccomp() {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == NULL)
        die("seccomp_init failed");

    // 네트워크 및 소켓 관련 시스템 콜 통제
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(connect), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(bind), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(listen), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(ptrace), 0);

    if (seccomp_load(ctx) < 0)
        die("seccomp_load failed");
    seccomp_release(ctx);
}

/*
 * fork를 통해 자식 프로세스를 생성하고 지정된 명령어를 실행함
 * 샌드박스 활성화 시 setrlimit과 seccomp를 적용해 자원(CPU, 메모리, 프로세스 수)을 제한하며 파이프 입출력을 리다이렉션함
 */
pid_t execute_process(const char *cmd, int fd_in, int fd_out, int is_sandboxed) {
    pid_t pid = fork();
    if (pid < 0)
        die("fork failed");
    
    if (pid == 0) {
        if (is_sandboxed) {
            struct rlimit p_limit = {10, 10}; // 프로세스 생성 한도
            if (setrlimit(RLIMIT_NPROC, &p_limit) == -1) die("RLIMIT_NPROC fail");

            struct rlimit f_limit = {1024 * 1024, 1024 * 1024}; // 파일 생성 크기 한도 (디스크 소모 방지)
            if (setrlimit(RLIMIT_FSIZE, &f_limit) == -1) die("RLIMIT_FSIZE fail");
            
            struct rlimit c_limit = { (TIME_LIMIT_MS / 1000) + 1, (TIME_LIMIT_MS / 1000) + 1 }; // CPU 사용 시간 한도
            if (setrlimit(RLIMIT_CPU, &c_limit) == -1) die("RLIMIT_CPU fail");
            
            struct rlimit m_limit = {1024 * 1024 * 1024, 1024 * 1024 * 1024}; // 메모리 할당 한도 1GB
            if (setrlimit(RLIMIT_AS, &m_limit) == -1) die("RLIMIT_AS fail");

            setup_seccomp();
        }

        // 파이프 파일 디스크립터를 표준 입출력으로 리다이렉션
        if (fd_in != STDIN_FILENO) {
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }
        if (fd_out != STDOUT_FILENO) {
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }
        execlp("sh", "sh", "-c", cmd, NULL);
        die("execlp failed");
    }
    return pid;
}

/*
 * CPU를 사용하지 않고 멈춰있는 악성 코드를 차단하기 위한 감시 스레드
 * 제한된 시간(Wall-clock Time) 초과 시 해당 프로세스에 SIGKILL을 전송하여 강제 종료함
 */
void* watchdog_thread(void* arg) {
    pid_t target = *(pid_t*)arg; // 감시 대상 자식 프로세스의 PID
    if (target <= 0)
        return NULL;

    struct timespec req = {
        .tv_sec = TIME_LIMIT_MS / 1000,
        .tv_nsec = (TIME_LIMIT_MS % 1000) * 1000000L,
    };

    nanosleep(&req, NULL);  // 밀리초를 마이크로초로 변환하여 대기
    if (target > 0)
        kill(target, SIGKILL);

    return NULL;
}

/*
 * poll 시스템 콜을 사용하여 두 개의 파이프를 비동기적으로 동시에 읽어들임
 * 한쪽 프로세스의 대량 출력으로 인한 버퍼 데드락을 방지하며 버퍼 용량을 초과한 데이터는 잘라낸 뒤 생략 기호를 덧붙임
 */
void read_pipes_async(int fd1, char *buf1, size_t size1, int fd2, char *buf2, size_t size2) {
    struct pollfd pfds[2]; // 다중 I/O 감시를 위한 구조체 배열
    pfds[0].fd = fd1;
    pfds[0].events = POLLIN;
    pfds[1].fd = fd2;
    pfds[1].events = POLLIN;

    size_t len1 = 0, len2 = 0; // 각 버퍼에 쓰인 현재 데이터 길이
    int trunc1 = 0, trunc2 = 0; // 잘림 발생 여부 플래그
    int active = 2; // 데이터를 읽기 위해 대기 중인 활성 파이프 개수
    
    memset(buf1, 0, size1);
    memset(buf2, 0, size2);

    while (active > 0) {
        if (poll(pfds, 2, POLL_TIMEOUT) <= 0) break;

        // 1. 정답 코드 파이프 읽기
        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            char temp[MAX_BUFFER + 1];
            ssize_t n = read(pfds[0].fd, temp, MAX_BUFFER);
            if (n > 0) {
                if (len1 + (size_t)n <= size1 - 15) {
                    memcpy(buf1 + len1, temp, n);
                    len1 += n; temp[n] = '\0';
                }
                else {
                    trunc1 = 1;
                }
            } else if (n < 0 && errno == EINTR) {
                // 시그널에 의해 방해받은 경우: 파이프를 닫지 않고 무시 (다시 poll을 돌게 됨)
            } else { // 진짜로 끝났거나 치명적 에러인 경우에만 닫음
                pfds[0].fd = -1;
                active--; 
            }
        }

        // 2. 제출 코드 파이프 읽기
        if (pfds[1].revents & (POLLIN | POLLHUP)) {
            char temp[MAX_BUFFER + 1];
            ssize_t n = read(pfds[1].fd, temp, MAX_BUFFER);
            if (n > 0) {
                if (len2 + (size_t)n <= size2 - 15) { 
                    memcpy(buf2 + len2, temp, n);
                    len2 += n; temp[n] = '\0';
                }
                else {
                    trunc2 = 1;
                }
            } else if (n < 0 && errno == EINTR) {}
            else {
                pfds[1].fd = -1;
                active--; 
            }
        }
    }
    // 잘린 버퍼 끝에 생략 기호 추가
    if (trunc1) strcat(buf1, "...truncated");
    if (trunc2) strcat(buf2, "...truncated");
}

/*
 * 반례 발생 시 동적 할당을 통해 이중 연결 리스트의 끝단에 새 노드를 추가함
 * 양방향 탐색을 지원하기 위해 새 노드의 prev 포인터에 이전 노드를 연결함
 */
void add_fail_case(FailNode **head, int test_num, ErrorType err_type, const char *in, const char *exp, const char *act) {
    FailNode *new_node = (FailNode *) malloc (sizeof(FailNode)); // 힙 영역에 할당할 새 실패 노드
    if (new_node == NULL) {
        die("malloc failed: out of memory");
    }

    new_node->test_number = test_num;
    new_node->error_type = err_type;
    
    strncpy(new_node->input, in, MAX_BUFFER - 1);
    new_node->input[MAX_BUFFER - 1] = '\0'; //문자열 끝 널문자 포함

    strncpy(new_node->expected, exp, MAX_BUFFER - 1);
    new_node->expected[MAX_BUFFER - 1] = '\0';

    strncpy(new_node->actual, act, MAX_BUFFER - 1);
    new_node->actual[MAX_BUFFER - 1] = '\0';
    new_node->prev = NULL;
    new_node->next = NULL;

    if (*head == NULL) {
        *head = new_node;
    } else {
        FailNode *curr = *head;
        while (curr->next != NULL)
            curr = curr->next;
        curr->next = new_node;
        new_node->prev = curr; 
    }
}

/*
 * 반례 뷰어 사용이 끝난 후 이중 연결 리스트 전체를 순회함
 * 동적 할당된 모든 노드의 메모리를 일괄 해제하고 헤드 포인터를 초기화함
 */
void free_fail_list(FailNode **head) {
    FailNode *curr = *head; // 리스트 순회를 위한 임시 포인터
    FailNode *temp;
    while (curr != NULL) {
        temp = curr;
        curr = curr->next;
        free(temp);
    }
    *head = NULL;
}

/*
 * 리스트에 수집된 모든 반례를 텍스트 파일로 영구 저장함
 * 에러 타입과 입출력 결과를 포맷팅하여 뷰어 종료 후에도 오답 원인을 분석할 수 있도록 파일에 기록함
 */
void export_fails_to_file(FailNode *head, const char *prob_num) {
    if (head == NULL)
        return;

    char filename[256] = {0}; // 생성할 로그 파일의 이름 버퍼
    snprintf(filename, sizeof(filename), "log_%s.txt", prob_num);

    FILE *fp = fopen(filename, "w");
    if (fp == NULL) return;

    fprintf(fp, "=================================================\n");
    fprintf(fp, "          문제 %s 번 채점 실패 오프라인 로그\n", prob_num);
    fprintf(fp, "=================================================\n\n");

    FailNode *curr = head;
    while (curr != NULL) {
        fprintf(fp, "[Test %d] ", curr->test_number);
        switch (curr->error_type) {
            case STATUS_WA:  fprintf(fp, "%s\n", STR_WA); break;
            case STATUS_TLE: fprintf(fp, "%s\n", STR_TLE); break;
            case STATUS_MLE: fprintf(fp, "%s\n", STR_MLE); break;
            case STATUS_RE:  fprintf(fp, "%s\n", STR_RE); break;
        }
        fprintf(fp, "-------------------------------------------------\n");
        fprintf(fp, "[입력 데이터]\n%s\n", curr->input);
        fprintf(fp, "[정답 출력 값]\n%s\n", curr->expected);
        if (curr->error_type == STATUS_WA) {
            fprintf(fp, "[제출 출력 값]\n%s\n", curr->actual);
        }
        fprintf(fp, "=================================================\n\n");
        curr = curr->next;
    }

    fclose(fp);
    printf("\n[알림] 전체 반례 로그가 '%s' 파일로 내보내졌음\n", filename);
    sleep(1);
}

/*
 * 터미널 속성을 프로그램 실행 직전의 원본 상태(표준 입력 모드)로 복구함
 * 방향키 기반 UI 탐색 종료 후 다시 일반적인 텍스트 입력이 가능하도록 전환함
 */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/*
 * 방향키 입력을 즉시 감지하기 위해 라인 버퍼링과 에코를 비활성화하여 비표준 모드로 전환함
 * 비정상 종료 시 터미널 먹통을 방지하기 위해 atexit 방어 코드를 등록함
 */
void enable_raw_mode() {
    static int atexit_registered = 0; // 정적 변수로 등록 여부 추적
    tcgetattr(STDIN_FILENO, &orig_termios);

    if (!atexit_registered) {
        atexit(disable_raw_mode); 
        atexit_registered = 1;
    }

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/*
 * 채점 종료 후 이스케이프 시퀀스를 이용해 터미널 기반 대화형 반례 뷰어를 실행함
 * 좌우 방향키 입력을 감지하여 이중 연결 리스트를 순회하며 화면을 갱신함
 */
void view_fails_interactive(FailNode *head) {
    if (head == NULL)
        return;

    FailNode *curr = head; // 현재 화면에 출력될 반례 노드 포인터
    int key; // 사용자 입력 키 값

    enable_raw_mode(); 

    while (1) {
        printf("\033[H\033[J"); 
        printf("============= 채점 결과 분석 (대화형 뷰어) =============\n");
        printf("[Test %d] 상태: ", curr->test_number);
        switch (curr->error_type) {
            case STATUS_WA:  printf("\033[1;31m%s\033[0m\n", STR_WA); break;
            case STATUS_TLE: printf("\033[1;33m%s\033[0m\n", STR_TLE); break;
            case STATUS_MLE: printf("\033[1;33m%s\033[0m\n", STR_MLE); break;
            case STATUS_RE:  printf("\033[1;35m%s\033[0m\n", STR_RE); break;
        }
        printf("-------------------------------------------------------\n");
        printf("[입력 내용]\n%s\n", curr->input);
        printf("[정답 출력]\n%s\n", curr->expected);
        if (curr->error_type == STATUS_WA) {
            printf("[제출 코드 출력]\n%s\n", curr->actual);
        }
        printf("-------------------------------------------------------\n");
        printf(" ◀ (이전): 왼쪽 방향키  |  ▶ (다음): 오른쪽 방향키 \n");
        printf(" 탐색 종료 및 메인 화면 복귀: Enter 키 또는 'q'\n");
        printf("=======================================================\n");

        key = getchar();
        if (key == '\n' || key == 'q' || key == 'Q') break;
        if (key == '\033') {
            int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
            fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
            
            int ch2 = getchar();
            int ch3 = getchar();
            fcntl(STDIN_FILENO, F_SETFL, flags); // 원래 상태(블로킹 모드)로 복구
            
            // 방향키 데이터가 맞는지 정상 검증 후 무빙
            if (ch2 == '[') {
                switch (ch3) {
                    case 'A': /*위 방향키*/ break;
                    case 'B': /*아래 방향키*/ break;
                    case 'C': if (curr->next != NULL) curr = curr->next; break; // 오른쪽
                    case 'D': if (curr->prev != NULL) curr = curr->prev; break; // 왼쪽
                }
            }
        }
    }
    disable_raw_mode(); 
    printf("\033[H\033[J");
}

/*
 * 초기 환경 설정 단계에서 상/하 방향키로 채점할 프로그래밍 언어를 선택하는 UI를 렌더링함
 * 비표준 모드에서 입력을 감지해 선택 인덱스를 갱신하며 ANSI 색상 코드로 현재 항목을 강조함
 */
int select_language() {
    const char *languages[] = {"C (gcc)", "C++ (g++)", "Python (python3)"};
    int num_langs = 3, selected = 0, key;

    enable_raw_mode(); 
    while (1) {
        printf("\033[H\033[J"); 
        printf(MSG_TITLE);
        printf(MSG_PROMPT_LANG);
        
        for (int i = 0; i < num_langs; i++) {
            if (i == selected)
                printf("  > \033[1;32m%s\033[0m <\n", languages[i]);
            else
                printf("    %s\n", languages[i]);
        }
        printf("\n=========================================\n");

        key = getchar();
        if (key == '\n')
            break;
        
        if (key == '\033') {
            getchar(); 
            switch(getchar()) {
                case 'A': selected = (selected - 1 + num_langs) % num_langs; break;
                case 'B': selected = (selected + 1) % num_langs; break;
            }
        }
    }
    disable_raw_mode(); 
    printf("\033[H\033[J"); 
    return selected;
}

/*
 * 고정 테스트케이스 파일과 동적 제너레이터를 조합해 대상 코드를 스트레스 테스트함
 * 파이프 IPC, poll 비동기 읽기, wait4를 이용한 리소스(메모리/시간) 검증을 수행하며 오답 발생 시 반례 리스트에 추가함
 */
void run_judge_session(const char *prob_num, int max_test_cases, const char *gen_cmd, const char *sol_cmd, const char *sub_cmd) {
    int test_count = 0; // 루프를 제어하는 현재 테스트 케이스 수행 횟수
    FailNode *fail_list_head = NULL; // 수집된 반례 리스트의 시작 포인터

    while (test_count < max_test_cases && !interrupted) {
        test_count++;
        
        char test_case[MAX_BUFFER] = {0}; // 파이프에 주입될 완성된 입력 데이터 버퍼
        char tc_path[256]; // 고정 테스트케이스 파일의 경로 조합 버퍼
        snprintf(tc_path, sizeof(tc_path), "%s/Testcase/%d.txt", prob_num, test_count);
        FILE *tc_file = fopen(tc_path, "r");

        if (tc_file) {
            fread(test_case, 1, sizeof(test_case) - 1, tc_file);
            fclose(tc_file);
        } else {
            int gen_pipe[2]; // 제너레이터 출력을 받을 파이프 배열
            if (pipe(gen_pipe) < 0)
                die("pipe error");
            
            gen_pid = execute_process(gen_cmd, STDIN_FILENO, gen_pipe[1], 0);
            close(gen_pipe[1]);
            
            char gen_ignore[MAX_BUFFER]; // 비동기 읽기 함수 구조에 맞추기 위한 더미 버퍼
            read_pipes_async(gen_pipe[0], test_case, sizeof(test_case), -1, gen_ignore, 0);
            close(gen_pipe[0]);
            waitpid(gen_pid, NULL, 0); 
            gen_pid = -1;
        }

        int sol_in[2], sol_out[2], sub_in[2], sub_out[2];
        if (pipe2(sol_in, O_CLOEXEC) < 0 || pipe2(sol_out, O_CLOEXEC) < 0 || 
            pipe2(sub_in, O_CLOEXEC) < 0 || pipe2(sub_out, O_CLOEXEC) < 0) {
            die("pipe error");
        }

        // 프로세스 실행 (제출 코드에만 샌드박스 적용)
        sol_pid = execute_process(sol_cmd, sol_in[0], sol_out[1], 0);
        sub_pid = execute_process(sub_cmd, sub_in[0], sub_out[1], 1); 

        close(sol_in[0]);
        close(sol_out[1]);
        close(sub_in[0]);
        close(sub_out[1]);
        
        write(sol_in[1], test_case, strlen(test_case));
        write(sub_in[1], test_case, strlen(test_case));
        close(sol_in[1]);
        close(sub_in[1]);

        pthread_t wd_tid; // 자식 프로세스의 Wall-clock 강제 종료를 담당할 스레드 ID
        pthread_create(&wd_tid, NULL, watchdog_thread, (void*)&sub_pid);

        char sol_result[MAX_BUFFER] = {0}; // 정답 코드가 반환한 최종 출력 버퍼
        char sub_result[MAX_BUFFER] = {0}; // 제출 코드가 반환한 최종 출력 버퍼
        
        read_pipes_async(sol_out[0], sol_result, sizeof(sol_result), sub_out[0], sub_result, sizeof(sub_result));
        
        close(sol_out[0]);
        close(sub_out[0]);

        pthread_cancel(wd_tid);
        pthread_join(wd_tid, NULL);

        if (waitpid(sol_pid, NULL, WNOHANG) == 0) { // 타임아웃 등으로 인해 아직 안 끝났다면 강제 종료
            kill(sol_pid, SIGKILL);
            waitpid(sol_pid, NULL, 0);
        }
        sol_pid = -1;
        
        int status; // 제출 프로세스의 반환 및 시그널 종료 상태 기록 변수
        struct rusage ru; // 커널로부터 회수된 시스템 자원(메모리 등) 사용량 구조체
        if (wait4(sub_pid, &status, WNOHANG, &ru) == 0) {
            kill(sub_pid, SIGKILL);
            wait4(sub_pid, &status, 0, &ru);
        }
        sub_pid = -1;

        int is_tle = 0, is_mle = 0, is_re = 0; // 에러 판별 플래그
        long mem_used_kb = ru.ru_maxrss; // 리눅스 환경의 최대 메모리 사용량 (KB 단위)

        if (mem_used_kb > MEM_LIMIT_KB)
            is_mle = 1;

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig == SIGXCPU || sig == SIGKILL)
                is_tle = 1;
            else
                is_re = 1;
        }

        // 결과 분류 및 반례 리스트 삽입
        if (is_tle) {
            add_fail_case(&fail_list_head, test_count, STATUS_TLE, test_case, sol_result, "");
        } else if (is_mle) {
            add_fail_case(&fail_list_head, test_count, STATUS_MLE, test_case, sol_result, "");
        } else if (is_re) {
            add_fail_case(&fail_list_head, test_count, STATUS_RE, test_case, sol_result, "");
        } else {
            if (strcmp(sol_result, sub_result) != 0) {
                add_fail_case(&fail_list_head, test_count, STATUS_WA, test_case, sol_result, sub_result);
            }
        }
    }

    if (!interrupted) {
        if (fail_list_head == NULL) {
            printf(MSG_ALL_PASS, test_count);
            printf(MSG_JUDGE_END);
            int ch;
            while ((ch = getchar()) != '\n' && ch != 'q' && ch != 'Q');
        } else {
            export_fails_to_file(fail_list_head, prob_num); 
            view_fails_interactive(fail_list_head);                       
        }
    }
    free_fail_list(&fail_list_head); 
}

/*
 * 프로그램의 진입점으로 메인 무한 루프를 돌며 사용자로부터 채점 정보를 입력받음
 * 입력된 정보에 맞춰 소스 코드를 사전 컴파일하며 디렉토리 아키텍처에 맞게 명령어를 동적 조립하여 채점 세션을 관장함
 */
int main() {
    setup_signals();

    while (1) {
        interrupted = 0; 
        
        char prob_num[32], file_path[256]; // 사용자 입력 문제 번호 및 파일 경로 저장 버퍼
        int max_test_cases = 50; // 사용자로부터 입력받은 검증 반복 횟수 기본값

        printf("\033[H\033[J");
        printf(MSG_INPUT_PROB);
        if (fgets(prob_num, sizeof(prob_num), stdin) != NULL) prob_num[strcspn(prob_num, "\n")] = 0;

        if (strcmp(prob_num, "q") == 0 || strcmp(prob_num, "Q") == 0) {
            printf(MSG_GOODBYE); break;
        }

        printf(MSG_INPUT_PATH);
        if (fgets(file_path, sizeof(file_path), stdin) != NULL)
            file_path[strcspn(file_path, "\n")] = 0;

        while (1) {
            char tc_input[32]; // 테스트케이스 개수 입력 임시 버퍼
            printf(MSG_INPUT_TC);
            if (fgets(tc_input, sizeof(tc_input), stdin) != NULL) {
                int parsed = atoi(tc_input);
                if (parsed >= 5 && parsed <= 3000) {
                    max_test_cases = parsed;
                    break;
                } else
                    printf("입력 오류: 5에서 3000 범위의 정수만 입력 가능함\n");
            }
        }

        int lang_choice = select_language(); // 사용자가 선택한 언어의 인덱스

        printf(MSG_SETTING_DONE);
        printf("- 문제 번호: %s\n", prob_num);
        printf("- 파일 경로: %s\n", file_path);
        printf("- 검증 횟수: 최대 %d회\n", max_test_cases);
        
        char sub_cmd[512] = {0}, compile_cmd[512] = {0}; // 실행할 명령어 동적 포맷팅 버퍼
        int compile_success = 1; // 사전 컴파일 성공 여부 플래그

        if (lang_choice == 0) { 
            printf("- 언어: C\n");
            snprintf(compile_cmd, sizeof(compile_cmd), CMD_FMT_GCC, file_path, BIN_TEMP_NAME);
            printf(MSG_COMPILE_ING);
            if (system(compile_cmd) == 0)
                snprintf(sub_cmd, sizeof(sub_cmd), "./%s", BIN_TEMP_NAME);
            else {
                printf(MSG_COMPILE_ERR);
                compile_success = 0;
            }
        } else if (lang_choice == 1) { 
            printf("- 언어: C++\n");
            snprintf(compile_cmd, sizeof(compile_cmd), CMD_FMT_GPP, file_path, BIN_TEMP_NAME);
            printf(MSG_COMPILE_ING);
            if (system(compile_cmd) == 0)
                snprintf(sub_cmd, sizeof(sub_cmd), "./%s", BIN_TEMP_NAME);
            else {
                printf(MSG_COMPILE_ERR);
                compile_success = 0;
            }
        } else { 
            printf("- 언어: Python\n");
            snprintf(sub_cmd, sizeof(sub_cmd), CMD_FMT_PYTHON, file_path);
        }

        if (compile_success) {
            char gen_cmd[256], sol_cmd[256]; // 제너레이터 및 정답 코드 명령어 조립 버퍼
            snprintf(gen_cmd, sizeof(gen_cmd), "./%s/Generators/gen", prob_num);
            snprintf(sol_cmd, sizeof(sol_cmd), "./%s/Correct/sol", prob_num);

            printf(MSG_JUDGE_START);
            sleep(1);

            run_judge_session(prob_num, max_test_cases, gen_cmd, sol_cmd, sub_cmd);

            // 임시 생성된 바이너리 파일 정리
            if (lang_choice == 0 || lang_choice == 1) remove(BIN_TEMP_NAME);
        }

        if (interrupted) {
            printf(MSG_JUDGE_ABORT);
            sleep(2);
        }
    }
    return 0;
}