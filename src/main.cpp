#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>

static void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

static void framebuffer_resize(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
}

int main() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "voxel_engine", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    std::printf("GL %d.%d  |  vendor=%s  |  renderer=%s\n",
                GLAD_VERSION_MAJOR(version),
                GLAD_VERSION_MINOR(version),
                glGetString(GL_VENDOR),
                glGetString(GL_RENDERER));

    glfwSetFramebufferSizeCallback(window, framebuffer_resize);

    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    double last_time = glfwGetTime();
    int frame_count = 0;

    while (!glfwWindowShouldClose(window)) {
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        glClearColor(0.10f, 0.45f, 0.55f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glfwSwapBuffers(window);
        glfwPollEvents();

        ++frame_count;
        double now = glfwGetTime();
        if (now - last_time >= 1.0) {
            char title[128];
            std::snprintf(title, sizeof(title), "voxel_engine  |  %d fps", frame_count);
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            last_time = now;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
