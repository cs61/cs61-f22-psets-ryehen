#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <iostream>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__

// struct command
//    Data structure describing a command. Add your own stuff.

struct command {
    std::vector<std::string> args;

    // Doubly linked list support
    command* next_in_pipeline = nullptr;
    command* prev_in_pipeline = nullptr;

    // Keep track of pid associated w process
    pid_t pid = -1;

    // Keeps track of redirected in / out / err as needed
    const char* c_in = nullptr;
    const char* c_out = nullptr;
    const char* c_err = nullptr;

    // Runs command
    bool run();

};

struct pipeline {
    // Support tree structure
    command* command_child = nullptr;
    
    // Doubly linked list support
    pipeline* next_in_conditional = nullptr;
    pipeline* prev_in_conditional = nullptr;

    // Keep track of logic relationship with adjacent pipeline
    bool next_is_or = false;
    bool prev_is_or = false;

    // Runs entire pipeline
    bool run();
};

struct conditional {
    // Support tree structure
    pipeline* pipeline_child = nullptr;

    // Doubly linked list support
    conditional* next_in_list = nullptr;
    conditional* prev_in_list = nullptr;

    // Keep track of whether run in foreground or background
    bool is_background = false;
};

// Helper function that performs redirection
void redirect(const char* file, int perms, int dest) {
    int fd = open(file, perms, 0644);

    if (fd == -1) {
        std::cerr << file << ": " << strerror(errno) << std::endl;
        _exit(EXIT_FAILURE);
    }

    dup2(fd, dest);
    close(fd);
}

// Helper function that changes directory
bool change_dir(const char* dir, const char* c_in, const char* c_out, const char* c_err) {
    // Execute redirects as needed
    int c_in_og;
    if (c_in) {
        c_in_og = dup(0);
        redirect(c_in, O_RDONLY, 0);
    }
    int c_out_og;
    if (c_out) {
        c_out_og = dup(1);
        redirect(c_out, O_WRONLY|O_CREAT, 1);
    }
    int c_err_og;
    if (c_err) {
        c_err_og = dup(2);
        redirect(c_err, O_WRONLY|O_CREAT, 2);
    }

    // Change directory
    int r = chdir(dir);

    // Notify user of error as needed
    if (r == -1) {
        std::cerr << "cd: " << dir << ": " << strerror(errno) << std::endl;
    }

    // Revert redirects as needed
    if (c_in) {
        dup2(c_in_og, 0);
    }
    if (c_out) {
        dup2(c_out_og, 1);
    }
    if (c_err) {
        dup2(c_err_og, 2);

    }
        
    return (r != -1);
}

// COMMAND EXECUTION

// command::run()
//    Creates a single child process running the command in `this`, and
//    sets `this->pid` to the pid of the child process.
//
//    If a child process cannot be created, this function should call
//    `_exit(EXIT_FAILURE)` (that is, `_exit(1)`) to exit the containing
//    shell or subshell. If this function returns to its caller,
//    `this->pid > 0` must always hold.
//
//    Note that this function must return to its caller *only* in the parent
//    process. The code that runs in the child process must `execvp` and/or
//    `_exit`.
//
//    PART 1: Fork a child process and run the command using `execvp`.
//       This will require creating a vector of `char*` arguments using
//       `this->args[N].c_str()`. Note that the last element of the vector
//       must be a `nullptr`.
//    PART 4: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PART 7: Handle redirections.

bool command::run() {
    // Check invariants
    assert(this->pid == -1);
    assert(this->args.size() > 0);

    // Format command arguments
    int n = this->args.size();
    const char* formatted_args[n + 1];
    formatted_args[n] = nullptr;

    for (int i = 0; i < (int) this->args.size(); i++) {
        formatted_args[i] = this->args[i].c_str();
    }

    // If the command is "cd", simply change directory
    if (strcmp(formatted_args[0], "cd") == 0) {
        return change_dir(formatted_args[1], this->c_in, this->c_out, this->c_err);
    }

    // Fork and execute command
    pid_t process = fork();
    if (process == 0) {
        // Execute redirects as needed
        if (this->c_in) {
            redirect(this->c_in, O_RDONLY, 0);
        }
        if (this->c_out) {
            redirect(this->c_out, O_WRONLY|O_CREAT, 1);
        }
        if (this->c_err) {
            redirect(this->c_err, O_WRONLY|O_CREAT, 2);
        }
        
        execvp(formatted_args[0], (char* const*) formatted_args);
        _exit(EXIT_FAILURE);
    }

    // Store process id
    this->pid = process;
    int status;

    // Wait for child process to finish
    pid_t w = waitpid(process, &status, 0);

    // Check invariants
    assert(w == process);
    assert(this->pid != -1);

    // Determine if execution was successful
    if (WIFEXITED(status)) {
        return (0 == WEXITSTATUS(status));
    } else {
        return false;
    }
}

bool pipeline::run() {
    
    // Initialize current command to first command in pipeline
    command* current_command = this->command_child;

    // Create initial pipe
    int pipe_fd[2];
    int r = pipe(pipe_fd);
    assert(r >= 0);

    // Initialize middle pipe to just be first pipe
    int pipe_fd_middle[2] = {pipe_fd[0], pipe_fd[1]};

    while (current_command != nullptr) {
        // Format command arguments
        int n = current_command->args.size();
        const char* formatted_args[n + 1];
        formatted_args[n] = nullptr;

        for (int i = 0; i < (int) current_command->args.size(); i++) {
            formatted_args[i] = current_command->args[i].c_str();
        }   
        
        // Prepare middle pipe as needed
        if (current_command->prev_in_pipeline != nullptr && 
            current_command->next_in_pipeline != nullptr) {
                r = pipe(pipe_fd_middle);
                assert(r>=0);
            }

        // Fork
        pid_t current_pid = fork();
        assert(current_pid >= 0);
    
        // If first command in pipeline...
        if (current_command->prev_in_pipeline == nullptr) {
            if (current_pid == 0) {
                // Execute redirects as needed
                if (current_command->c_in) {
                    redirect(current_command->c_in, O_RDONLY, 0);
                }
                if (current_command->c_out) {
                    redirect(current_command->c_out, O_WRONLY|O_CREAT, 1);
                } else {
                    dup2(pipe_fd[1], 1);
                }
                if (current_command->c_err) {
                    redirect(current_command->c_err, O_WRONLY|O_CREAT, 2);
                }

                close(pipe_fd[0]);
                close(pipe_fd[1]);
                r = execvp(formatted_args[0], (char* const*) formatted_args);
                _exit(EXIT_FAILURE);
            }

            close(pipe_fd[1]);
        
        // If in middle of pipeline...
        } else if (current_command->next_in_pipeline != nullptr) {
                if (current_pid == 0) {
                    // Execute redirects as needed
                    if (current_command->c_in) {
                        redirect(current_command->c_in, O_RDONLY, 0);
                    } else {
                        dup2(pipe_fd[0], 0);
                    }
                    if (current_command->c_out) {
                        redirect(current_command->c_out, O_WRONLY|O_CREAT, 1);
                    } else {
                        dup2(pipe_fd_middle[1], 1);
                    }
                    if (current_command->c_err) {
                        redirect(current_command->c_err, O_WRONLY|O_CREAT, 2);
                    }

                    close(pipe_fd[0]);
                    close(pipe_fd_middle[0]);
                    close(pipe_fd_middle[1]);
                    r = execvp(formatted_args[0], (char* const*) formatted_args);
                    _exit(EXIT_FAILURE);
                }

                close(pipe_fd[0]);
                close(pipe_fd_middle[1]);

                // If it's not the last middle pipe, update pipe_fd to be the previous middle pipe
                if (current_command->next_in_pipeline->next_in_pipeline != nullptr) {
                    pipe_fd[0] = pipe_fd_middle[0];
                    pipe_fd[1] = pipe_fd_middle[1];
                }
        // If at end of pipeline...
        } else {
            if (current_pid == 0) {
                // Execute redirects as needed
                if (current_command->c_in) {
                    redirect(current_command->c_in, O_RDONLY, 0);
                } else {
                    dup2(pipe_fd_middle[0], 0);
                }
                if (current_command->c_out) {
                    redirect(current_command->c_out, O_WRONLY|O_CREAT, 1);
                }
                if (current_command->c_err) {
                    redirect(current_command->c_err, O_WRONLY|O_CREAT, 2);
                }
                
                close(pipe_fd_middle[0]);
                r = execvp(formatted_args[0], (char* const*) formatted_args);
                _exit(EXIT_FAILURE);
            }

            close(pipe_fd_middle[0]);
            int status;
            pid_t w = waitpid(current_pid, &status, 0);
            assert(w == current_pid);

            // Determine if execution was successful
            if (WIFEXITED(status)) {
                return (0 == WEXITSTATUS(status));
            }
        }

        // Update current command to next in pipeline
        current_command = current_command->next_in_pipeline;
    }

    return false;
}

// run_list(c)
//    Run the command *list* starting at `c`. Initially this just calls
//    `c->run()` and `waitpid`; you’ll extend it to handle command lists,
//    conditionals, and pipelines.
//
//    It is possible, and not too ugly, to handle lists, conditionals,
//    *and* pipelines entirely within `run_list`, but many students choose
//    to introduce `run_conditional` and `run_pipeline` functions that
//    are called by `run_list`. It’s up to you.
//
//    PART 1: Start the single command `c` with `c->run()`,
//        and wait for it to finish using `waitpid`.
//    The remaining parts may require that you change `struct command`
//    (e.g., to track whether a command is in the background)
//    and write code in `command::run` (or in helper functions).
//    PART 2: Introduce a loop to run a list of commands, waiting for each
//       to finish before going on to the next.
//    PART 3: Change the loop to handle conditional chains.
//    PART 4: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PART 5: Change the loop to handle background conditional chains.
//       This may require adding another call to `fork()`!

void run_list(conditional* head_conditional) {
    // Initialize pointers
    conditional* current_conditional = head_conditional;
    pipeline* current_pipeline = current_conditional->pipeline_child;
    command* current_command = current_pipeline->command_child;

    // Keeps track of most recent exit status for conditonal's sake
    bool prev_exit_status = false;
    pid_t current_pid = -1;

    while(current_conditional != nullptr) {
        while (current_pipeline != nullptr) {
            // Create subshell for background pipelines as needed
            if (current_conditional->is_background && current_pid != 0) {
                current_pid = fork();

                // Parent shell moves on to next conditional
                if (current_pid != 0) {
                    break;
                }
            }

            // Only call command run if it's the only command in pipeline
            if (current_command->next_in_pipeline == nullptr) {
                prev_exit_status = current_command->run();
            // Otherwise we run entire pipeline
            } else {
                prev_exit_status = current_pipeline->run();
            }

            // Determine whether next pipeline gets run
            // Contingent on exit status and whether linked by '&&' or '||'
            if (prev_exit_status) {
                if (current_pipeline->next_is_or) {
                    // Find first pipeline not linked by '||'
                    current_pipeline = current_pipeline->next_in_conditional;
                    while (current_pipeline != nullptr) {
                        if (!current_pipeline->prev_is_or) {
                            break;
                        }
                        current_pipeline = current_pipeline->next_in_conditional;
                    }
                } else {
                    current_pipeline = current_pipeline->next_in_conditional;
                }
            } else {
                if (current_pipeline->next_is_or) {
                    current_pipeline = current_pipeline->next_in_conditional;
                // Find first pipeline not linked by '&&'
                } else {
                    current_pipeline = current_pipeline->next_in_conditional;
                    while (current_pipeline != nullptr) {
                        if (current_pipeline->prev_is_or) {
                            break;
                        }
                        current_pipeline = current_pipeline->next_in_conditional;
                    }
                }
            }

            // Update current command as needed
            if (current_pipeline != nullptr) {
                current_command = current_pipeline->command_child;
            } else {
                current_command = nullptr;
            }
        }
        // Exit subshell as needed
        if (current_pid == 0) {
            if (prev_exit_status) {
                _exit(EXIT_SUCCESS);
            } else {
                _exit(EXIT_FAILURE);
            }
        }

        // Parent moves on to next conditional
        current_conditional = current_conditional->next_in_list;
        if (current_conditional != nullptr) {
            current_pipeline = current_conditional->pipeline_child;
            current_command = current_pipeline->command_child;
        }
    }
    
}

// parse_line(s)
//    Parse the command list in `s` and return it. Returns `nullptr` if
//    `s` is empty (only spaces). You’ll extend it to handle more token
//    types.

conditional* parse_line(const char* s) {
    shell_parser parser(s);
    // Your code here!

    // Build the command
    // The handout code treats every token as a normal command word.
    // You'll add code to handle operators.
    // Initialize new command
    command* current_command = nullptr;
    pipeline* current_pipeline = nullptr;
    conditional* current_conditional = nullptr;
    conditional* head_conditional = nullptr;
    for (shell_token_iterator it = parser.begin(); it != parser.end(); ++it) {
        if (!current_conditional) {
            head_conditional = current_conditional = new conditional;
            current_conditional->pipeline_child = new pipeline;
            current_pipeline = current_conditional->pipeline_child;
            current_pipeline->command_child = new command;
            current_command = current_pipeline->command_child;

            // current_pipeline = current_conditional->pipeline_child = current_pipeline;
            // current_command = current_conditional->pipeline_child->command_child = current_command;
        }
        
        // Start new command when we encounter "|"
        if (it.type() == TYPE_PIPE) {
            // Create space for next command
            current_command->next_in_pipeline = new command;

            // Store pointer to prev command in next command
            current_command->next_in_pipeline->prev_in_pipeline = current_command;

            // Update 'c' to point to next command
            current_command = current_command->next_in_pipeline;
        }

        // Start new pipeline when we encounter '&&' or '||'
        if (it.type() == TYPE_AND || it.type() == TYPE_OR) {
            // Create space for next pipeline
            current_pipeline->next_in_conditional = new pipeline;
            current_pipeline->next_in_conditional->command_child = new command;

            if (it.type() == TYPE_OR) {
                current_pipeline->next_is_or = true;
                current_pipeline->next_in_conditional->prev_is_or = true;
            }

            // Store pointer to prev pipeline in next pipeline
            current_pipeline->next_in_conditional->prev_in_conditional = current_pipeline;

            // Update 'current_pipline' to point to next pipeline
            current_pipeline = current_pipeline->next_in_conditional;

            // Update 'c' to point to command in new pipeline
            current_command = current_pipeline->command_child;
        }

        if (it.type() == TYPE_BACKGROUND) {
            current_conditional->is_background = true;
        }

        shell_token_iterator next_it = it;
        ++next_it;
        // Start new conditional when we encounter ';' or '&'
        if ((next_it != parser.end()) && ((it.type() == TYPE_SEQUENCE) || (it.type() == TYPE_BACKGROUND))) {
            // Create space for next conditional
            current_conditional->next_in_list = new conditional;
            current_conditional->next_in_list->pipeline_child = new pipeline;
            current_conditional->next_in_list->pipeline_child->command_child = new command;

            // Store pointer to prev conditional in next conditional
            current_conditional->next_in_list->prev_in_list = current_conditional;

            // Update 'current_conditional' to point to next conditional
            current_conditional = current_conditional->next_in_list;

            // Update 'current_pipeline' to pipeline in next conditional
            current_pipeline = current_conditional->pipeline_child;

            // Update 'c' to point to command in next conditional's pipeline
            current_command = current_pipeline->command_child;

        }

        if (it.type() == TYPE_NORMAL) {
            current_command->args.push_back(it.str());
        }

        // Adjust c_in / c_out / c_err as needed when encountering redirect operator
        if (it.type() == TYPE_REDIRECT_OP) {
            shell_token_iterator prev_it = it;
            ++it;
            if (it.type() == TYPE_NORMAL) {
                if (prev_it.str() == "<") {
                    current_command->c_in = (char*) malloc(strlen(it.str().c_str()) + 1);
                    strcpy((char*) current_command->c_in, it.str().c_str()); 
                } else if (prev_it.str() == ">") {
                    current_command->c_out = (char*) malloc(strlen(it.str().c_str()) + 1);
                    strcpy((char*) current_command->c_out, it.str().c_str()); 
                } else {
                    current_command->c_err = (char*) malloc(strlen(it.str().c_str()) + 1);
                    strcpy((char*) current_command->c_err, it.str().c_str()); 
                }
            }
        }
    }
    return head_conditional;
}

void delete_tree(conditional* head_conditional) {
    // Initialize pointers
    conditional* current_conditional = head_conditional;
    pipeline* current_pipeline = current_conditional->pipeline_child;
    command* current_command = current_pipeline->command_child;

    // Delete every command per pipeline, every pipeline per conditional, then every conditional in the list
    while(current_conditional != nullptr) {
        while (current_pipeline != nullptr) {
            while (current_command != nullptr) {
                command* next_command = current_command->next_in_pipeline;
                free((void*) current_command->c_in);
                free((void*) current_command->c_out);
                free((void*) current_command->c_err);
                delete current_command;
                
                current_command = next_command;
            }
            pipeline* next_pipeline = current_pipeline->next_in_conditional;
            delete current_pipeline;
            
            current_pipeline = next_pipeline;
            if (current_pipeline) {
                current_command = current_pipeline->command_child;
            }
        }
        conditional* next_conditional = current_conditional->next_in_list;
        delete current_conditional;
        
        current_conditional = next_conditional;
        if (current_conditional) {
            current_pipeline = current_conditional->pipeline_child;
            current_command = current_pipeline->command_child;
        }
    }
}

void shoot_zombies() {
    pid_t cpid;
    int status;
    while ((cpid = waitpid(-1, &status, WNOHANG)) > 0) {}
}


int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (conditional* head_conditional = parse_line(buf)) {
                run_list(head_conditional);
                delete_tree(head_conditional);
            }
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes and/or interrupt requests
        shoot_zombies();
    }

    return 0;
}
