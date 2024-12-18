#include <stdlib.h>   
#include <unistd.h>   
#include <stdio.h>    
#include <string.h>   
#include <sys/wait.h> 
#include <fcntl.h>    
#include <errno.h>    

#define SHELL_ERROR "An error has occurred\n"
#define BUFFER_SIZE 514
#define SHELL_PROMPT "myshell> "

typedef struct {
    char **command_parts;
    char *redirect_file;
    int redirect_mode;
} shell_command;

void write_output(char *msg) {
    write(STDOUT_FILENO, msg, strlen(msg));
}

void handle_error(void) {
    write_output(SHELL_ERROR);
}

int check_empty_line(char *line) {
    char *ptr = line;
    while (*ptr) {
        if (*ptr != ' ' && *ptr != '\t') return 0;
        ptr++;
    }
    return 1;
}

char *process_input_line(FILE *input_source, int batch_mode) {
    char *buffer = malloc(BUFFER_SIZE + 2);
    if (!buffer) {
        handle_error();
        return NULL;
    }

    if (batch_mode) {
        int chars_read = 0;
        int max_size = BUFFER_SIZE + 2;
        char *complete_line = malloc(max_size);
        
        if (!complete_line) {
            free(buffer);
            handle_error();
            return NULL;
        }
        
        int current_char;
        while ((current_char = fgetc(input_source)) != EOF && current_char != '\n') {
            if (chars_read + 1 >= max_size) {
                max_size *= 2;
                char *temp = realloc(complete_line, max_size);
                if (!temp) {
                    free(complete_line);
                    free(buffer);
                    handle_error();
                    return NULL;
                }
                complete_line = temp;
            }
            complete_line[chars_read++] = current_char;
        }

        if (chars_read == 0 && current_char == EOF) {
            free(complete_line);
            free(buffer);
            return NULL;
        }

        complete_line[chars_read] = '\0';

        if (!check_empty_line(complete_line)) {
            char *print_line = malloc(chars_read + 2);
            strcpy(print_line, complete_line);
            strcat(print_line, "\n");
            write_output(print_line);
            free(print_line);
        }

        if (chars_read > BUFFER_SIZE) {
            handle_error();
            free(complete_line);
            free(buffer);
            return NULL;
        }

        strcpy(buffer, complete_line);
        free(complete_line);
    } else {
        if (!fgets(buffer, BUFFER_SIZE + 2, input_source)) {
            free(buffer);
            return NULL;
        }

        size_t len = strlen(buffer);
        if (len > BUFFER_SIZE + 1) {
            handle_error();
            free(buffer);
            return NULL;
        }
    }

    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }
    return buffer;
}
void execute_builtin_cd(char *path) {
    char *target = path ? path : getenv("HOME");
    if (chdir(target) != 0) handle_error();
}

void execute_builtin_pwd(void) {
    char current_dir[BUFFER_SIZE];
    if (getcwd(current_dir, sizeof(current_dir))) {
        strcat(current_dir, "\n");
        write_output(current_dir);
    } else {
        handle_error();
    }
}

void handle_standard_redirection(shell_command *cmd) {
    if (access(cmd->redirect_file, F_OK) == 0) {
        handle_error();
        _exit(1);
    }
    
    int fd = open(cmd->redirect_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
        if (fd >= 0) close(fd);
        handle_error();
        _exit(1);
    }
    close(fd);
}

void handle_advanced_redirection(shell_command *cmd) {
    char *old_content = NULL;
    size_t old_size = 0;
    int existing_fd = open(cmd->redirect_file, O_RDONLY);
    
    if (existing_fd >= 0) {
        old_size = lseek(existing_fd, 0, SEEK_END);
        lseek(existing_fd, 0, SEEK_SET);
        
        if (old_size > 0) {
            old_content = malloc(old_size);
            if (old_content) old_size = read(existing_fd, old_content, old_size);
        }
        close(existing_fd);
    }

    int out_fd = open(cmd->redirect_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        free(old_content);
        handle_error();
        _exit(1);
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        close(out_fd);
        free(old_content);
        handle_error();
        _exit(1);
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(out_fd);
        free(old_content);
        handle_error();
        _exit(1);
    }

    if (child_pid == 0) {
        close(pipe_fds[0]);
        close(out_fd);
        
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            close(pipe_fds[1]);
            handle_error();
            _exit(1);
        }
        close(pipe_fds[1]);
        
        execvp(cmd->command_parts[0], cmd->command_parts);
        handle_error();
        _exit(1);
    }

    close(pipe_fds[1]);
    
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (write(out_fd, buffer, bytes) != bytes) {
            close(pipe_fds[0]);
            close(out_fd);
            free(old_content);
            handle_error();
            _exit(1);
        }
    }

    if (old_content && old_size > 0) {
        if (write(out_fd, old_content, old_size) != old_size) {
            close(pipe_fds[0]);
            close(out_fd);
            free(old_content);
            handle_error();
            _exit(1);
        }
    }

    free(old_content);
    close(pipe_fds[0]);
    close(out_fd);
    
    int status;
    waitpid(child_pid, &status, 0);
    _exit(WEXITSTATUS(status));
}
shell_command parse_shell_command(char *input) {
    shell_command result = {NULL, NULL, 0};
    result.command_parts = malloc(BUFFER_SIZE * sizeof(char*));
    
    char *redirect = strstr(input, ">");
    if (redirect) {
        char *second_redirect = strstr(redirect + 1, ">");
        if (second_redirect) {
            free(result.command_parts);
            result.command_parts = NULL;
            return result;
        }

        *redirect = '\0';
        char *output_start = redirect + 1;
        
        if (*output_start == '+') {
            result.redirect_mode = 2;
            output_start++;
        } else {
            result.redirect_mode = 1;
        }

        while (*output_start == ' ' || *output_start == '\t') output_start++;

        if (!*output_start) {
            free(result.command_parts);
            result.command_parts = NULL;
            return result;
        }

        char *output_end = output_start + strlen(output_start) - 1;
        while (output_end > output_start && (*output_end == ' ' || *output_end == '\t')) {
            output_end--;
        }
        *(output_end + 1) = '\0';

        char *space_check = output_start;
        while (*space_check) {
            if (*space_check == ' ' || *space_check == '\t') {
                free(result.command_parts);
                result.command_parts = NULL;
                return result;
            }
            space_check++;
        }

        result.redirect_file = strdup(output_start);
    }

    int position = 0;
    char *arg = strtok(input, " \t");
    while (arg && position < BUFFER_SIZE - 1) {
        result.command_parts[position] = strdup(arg);
        position++;
        arg = strtok(NULL, " \t");
    }
    result.command_parts[position] = NULL;

    if (!result.command_parts[0]) {
        free(result.command_parts);
        result.command_parts = NULL;
        if (result.redirect_file) {
            free(result.redirect_file);
            result.redirect_file = NULL;
        }
    }
    
    return result;
}

void execute_command(shell_command cmd) {
    if (!cmd.command_parts || !cmd.command_parts[0]) return;

    if (strcmp(cmd.command_parts[0], "exit") == 0) {
        if (cmd.command_parts[1] || cmd.redirect_file) {
            handle_error();
            return;
        }
        exit(0);
    }
    
    if (strcmp(cmd.command_parts[0], "cd") == 0) {
        if (cmd.redirect_file || (cmd.command_parts[1] && cmd.command_parts[2])) {
            handle_error();
            return;
        }
        execute_builtin_cd(cmd.command_parts[1]);
        return;
    }
    
    if (strcmp(cmd.command_parts[0], "pwd") == 0) {
        if (cmd.command_parts[1] || cmd.redirect_file) {
            handle_error();
            return;
        }
        execute_builtin_pwd();
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        handle_error();
        return;
    }

    if (pid == 0) {
        if (cmd.redirect_file) {
            if (cmd.redirect_mode == 1) {
                handle_standard_redirection(&cmd);
            } else {
                handle_advanced_redirection(&cmd);
            }
        }
        execvp(cmd.command_parts[0], cmd.command_parts);
        handle_error();
        _exit(1);
    }

    waitpid(pid, NULL, 0);
}

void execute_command_sequence(char *input) {
    char *current = input;

    while (*current) {
        while (*current == ' ' || *current == '\t') current++;
        
        if (!*current) break;

        char *next = current;
        while (*next && *next != ';') next++;

        int length = next - current;
        if (length > 0) {
            char *single_command = malloc(length + 1);
            memcpy(single_command, current, length);
            single_command[length] = '\0';

            char *end = single_command + length - 1;
            while (end >= single_command && (*end == ' ' || *end == '\t')) {
                *end = '\0';
                end--;
            }

            if (!check_empty_line(single_command)) {
                shell_command cmd = parse_shell_command(single_command);
                if (cmd.command_parts) {
                    execute_command(cmd);
                    free(cmd.command_parts);
                    if (cmd.redirect_file) free(cmd.redirect_file);
                } else {
                    handle_error();
                }
            }

            free(single_command);
        }

        if (!*next) break;
        current = next + 1;
    }
}
void run_shell_loop(FILE *input_source, int batch_mode) {
    while (1) {
        if (!batch_mode) write_output(SHELL_PROMPT);

        char *command_line = process_input_line(input_source, batch_mode);
        if (!command_line) {
            if (feof(input_source)) break;
            continue;
        }

        size_t input_len = strlen(command_line);
        
        if (input_len > 512) {
            handle_error();
            free(command_line);
            continue;
        }

        if (!check_empty_line(command_line)) {
            execute_command_sequence(command_line);
        }
        
        free(command_line);
    }
}

int main(int argc, char *argv[]) {
    if (argc > 2) {
        handle_error();
        exit(1);
    }

    if (argc == 1) {
        run_shell_loop(stdin, 0);
    } else {
        FILE *batch_file = fopen(argv[1], "r");
        if (!batch_file) {
            handle_error();
            exit(1);
        }

        run_shell_loop(batch_file, 1);
        fclose(batch_file);
    }

    return 0;
}