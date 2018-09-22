
#define HAVE_OPENGL
#include "SDL2/SDL.h"

#include "GL/glew.h"
#include "SDL2/SDL_opengl.h"

/* RTL-SDR */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "rtl-sdr.h"
#include <time.h>

#include "convenience.h"

#define DEFAULT_SAMPLE_RATE		2048000
#define DEFAULT_BUF_LENGTH		(1* 4096)
#define MINIMAL_BUF_LENGTH		512
#define MAXIMAL_BUF_LENGTH		(256 * 16384)

#define MHZ(x)	((x)*1000*1000)

#define PPM_DURATION			10
#define PPM_DUMP_TIME			5

/* SDR vars */
static rtlsdr_dev_t *dev = NULL;

static uint32_t samp_rate = DEFAULT_SAMPLE_RATE;

static uint32_t total_samples = 0;
static uint32_t dropped_samples = 0;

/* Shader vars */
static SDL_bool shaders_supported;
static int      current_shader = 0;
enum {
    SHADER_COLOR,
    NUM_SHADERS
};

typedef struct {
    GLhandleARB program;
    GLhandleARB vert_shader;
    GLhandleARB frag_shader;
    const char *vert_source;
    const char *frag_source;
} ShaderData;

static ShaderData shaders[NUM_SHADERS] = {

    /* SHADER_COLOR */
    { 0, 0, 0,
        /* vertex shader */
	    "#version 130\n"
"in vec2 x_pos;\n"
"in vec3 x_color;\n"
"out vec3 v_color;\n"
"\n"
"void main()\n"
"{\n"
"    gl_Position = vec4(x_pos.x,x_pos.y,0,1.0);\n"//gl_ModelViewProjectionMatrix * gl_Vertex;\n"
"    v_color = x_color;\n"
"}",
        /* fragment shader */
"#version 130\n"
"in vec3 v_color;\n"
"\n"
"void main()\n"
"{\n"
"    gl_FragColor = vec4(v_color;\n"
"}"
    },
};

static PFNGLATTACHOBJECTARBPROC glAttachObjectARB;
static PFNGLCOMPILESHADERARBPROC glCompileShaderARB;
static PFNGLCREATEPROGRAMOBJECTARBPROC glCreateProgramObjectARB;
static PFNGLCREATESHADEROBJECTARBPROC glCreateShaderObjectARB;
static PFNGLDELETEOBJECTARBPROC glDeleteObjectARB;
static PFNGLGETINFOLOGARBPROC glGetInfoLogARB;
static PFNGLGETOBJECTPARAMETERIVARBPROC glGetObjectParameterivARB;
static PFNGLGETUNIFORMLOCATIONARBPROC glGetUniformLocationARB;
static PFNGLLINKPROGRAMARBPROC glLinkProgramARB;
static PFNGLSHADERSOURCEARBPROC glShaderSourceARB;
static PFNGLUNIFORM1IARBPROC glUniform1iARB;
static PFNGLUSEPROGRAMOBJECTARBPROC glUseProgramObjectARB;

static SDL_bool CompileShader(GLhandleARB shader, const char *source)
{
    GLint status;

    glShaderSourceARB(shader, 1, &source, NULL);
    glCompileShaderARB(shader);
    glGetObjectParameterivARB(shader, GL_OBJECT_COMPILE_STATUS_ARB, &status);
    if (status == 0) {
        GLint length;
        char *info;

        glGetObjectParameterivARB(shader, GL_OBJECT_INFO_LOG_LENGTH_ARB, &length);
        info = SDL_stack_alloc(char, length+1);
        glGetInfoLogARB(shader, length, NULL, info);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to compile shader:\n%s\n%s", source, info);
        SDL_stack_free(info);

        return SDL_FALSE;
    } else {
        return SDL_TRUE;
    }
}

static SDL_bool CompileShaderProgram(ShaderData *data)
{
    const int num_tmus_bound = 4;
    int i;
    GLint location;

    glGetError();

    /* Create one program object to rule them all */
    data->program = glCreateProgramObjectARB();

    /* Create the vertex shader */
    data->vert_shader = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
    if (!CompileShader(data->vert_shader, data->vert_source)) {
        return SDL_FALSE;
    }

    /* Create the fragment shader */
    data->frag_shader = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
    if (!CompileShader(data->frag_shader, data->frag_source)) {
        return SDL_FALSE;
    }

    /* ... and in the darkness bind them */
    glAttachObjectARB(data->program, data->vert_shader);
    glAttachObjectARB(data->program, data->frag_shader);
    glLinkProgramARB(data->program);

    /* Set up some uniform variables */
    glUseProgramObjectARB(data->program);
    for (i = 0; i < num_tmus_bound; ++i) {
        char tex_name[5];
        SDL_snprintf(tex_name, SDL_arraysize(tex_name), "tex%d", i);
        location = glGetUniformLocationARB(data->program, tex_name);
        if (location >= 0) {
            glUniform1iARB(location, i);
        }
    }
    glUseProgramObjectARB(0);

    return (glGetError() == GL_NO_ERROR) ? SDL_TRUE : SDL_FALSE;
}

static void DestroyShaderProgram(ShaderData *data)
{
    if (shaders_supported) {
        glDeleteObjectARB(data->vert_shader);
        glDeleteObjectARB(data->frag_shader);
        glDeleteObjectARB(data->program);
    }
}

static SDL_bool InitShaders()
{
    int i;

    /* Check for shader support */
    shaders_supported = SDL_FALSE;
    if (SDL_GL_ExtensionSupported("GL_ARB_shader_objects") &&
        SDL_GL_ExtensionSupported("GL_ARB_shading_language_100") &&
        SDL_GL_ExtensionSupported("GL_ARB_vertex_shader") &&
        SDL_GL_ExtensionSupported("GL_ARB_fragment_shader")) {
        glAttachObjectARB = (PFNGLATTACHOBJECTARBPROC) SDL_GL_GetProcAddress("glAttachObjectARB");
        glCompileShaderARB = (PFNGLCOMPILESHADERARBPROC) SDL_GL_GetProcAddress("glCompileShaderARB");
        glCreateProgramObjectARB = (PFNGLCREATEPROGRAMOBJECTARBPROC) SDL_GL_GetProcAddress("glCreateProgramObjectARB");
        glCreateShaderObjectARB = (PFNGLCREATESHADEROBJECTARBPROC) SDL_GL_GetProcAddress("glCreateShaderObjectARB");
        glDeleteObjectARB = (PFNGLDELETEOBJECTARBPROC) SDL_GL_GetProcAddress("glDeleteObjectARB");
        glGetInfoLogARB = (PFNGLGETINFOLOGARBPROC) SDL_GL_GetProcAddress("glGetInfoLogARB");
        glGetObjectParameterivARB = (PFNGLGETOBJECTPARAMETERIVARBPROC) SDL_GL_GetProcAddress("glGetObjectParameterivARB");
        glGetUniformLocationARB = (PFNGLGETUNIFORMLOCATIONARBPROC) SDL_GL_GetProcAddress("glGetUniformLocationARB");
        glLinkProgramARB = (PFNGLLINKPROGRAMARBPROC) SDL_GL_GetProcAddress("glLinkProgramARB");
        glShaderSourceARB = (PFNGLSHADERSOURCEARBPROC) SDL_GL_GetProcAddress("glShaderSourceARB");
        glUniform1iARB = (PFNGLUNIFORM1IARBPROC) SDL_GL_GetProcAddress("glUniform1iARB");
        glUseProgramObjectARB = (PFNGLUSEPROGRAMOBJECTARBPROC) SDL_GL_GetProcAddress("glUseProgramObjectARB");
        if (glAttachObjectARB &&
            glCompileShaderARB &&
            glCreateProgramObjectARB &&
            glCreateShaderObjectARB &&
            glDeleteObjectARB &&
            glGetInfoLogARB &&
            glGetObjectParameterivARB &&
            glGetUniformLocationARB &&
            glLinkProgramARB &&
            glShaderSourceARB &&
            glUniform1iARB &&
            glUseProgramObjectARB) {
            shaders_supported = SDL_TRUE;
        }
    }

    if (!shaders_supported) {
        return SDL_FALSE;
    }

    /* Compile all the shaders */
    for (i = 0; i < NUM_SHADERS; ++i) {
        if (!CompileShaderProgram(&shaders[i])) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to compile shader!\n");
            return SDL_FALSE;
        }
    }

    /* We're done! */
    return SDL_TRUE;
}

static void QuitShaders()
{
    int i;

    for (i = 0; i < NUM_SHADERS; ++i) {
        DestroyShaderProgram(&shaders[i]);
    }
}

/* Quick utility function for texture creation */
static int
power_of_two(int input)
{
    int value = 1;

    while (value < input) {
        value <<= 1;
    }
    return value;
}

GLuint
SDL_GL_LoadTexture(SDL_Surface * surface, GLfloat * texcoord)
{
    GLuint texture;
    int w, h;
    SDL_Surface *image;
    SDL_Rect area;
    SDL_BlendMode saved_mode;

    /* Use the surface width and height expanded to powers of 2 */
    w = power_of_two(surface->w);
    h = power_of_two(surface->h);
    texcoord[0] = 0.0f;         /* Min X */
    texcoord[1] = 0.0f;         /* Min Y */
    texcoord[2] = (GLfloat) surface->w / w;     /* Max X */
    texcoord[3] = (GLfloat) surface->h / h;     /* Max Y */

    image = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32,
#if SDL_BYTEORDER == SDL_LIL_ENDIAN     /* OpenGL RGBA masks */
                                 0x000000FF,
                                 0x0000FF00, 0x00FF0000, 0xFF000000
#else
                                 0xFF000000,
                                 0x00FF0000, 0x0000FF00, 0x000000FF
#endif
        );
    if (image == NULL) {
        return 0;
    }

    /* Save the alpha blending attributes */
    SDL_GetSurfaceBlendMode(surface, &saved_mode);
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);

    /* Copy the surface into the GL texture image */
    area.x = 0;
    area.y = 0;
    area.w = surface->w;
    area.h = surface->h;
    SDL_BlitSurface(surface, &area, image, &area);

    /* Restore the alpha blending attributes */
    SDL_SetSurfaceBlendMode(surface, saved_mode);

    /* Create an OpenGL texture for the image */
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    SDL_FreeSurface(image);     /* No longer needed */

    return texture;
}

/* A general OpenGL initialization function.    Sets all of the initial parameters. */
void InitGL(int Width, int Height)                    /* We call this right after our OpenGL window is created. */
{
    GLdouble aspect;

    glViewport(0, 0, Width, Height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);        /* This Will Clear The Background Color To Black */
    glClearDepth(1.0);                /* Enables Clearing Of The Depth Buffer */
    glDepthFunc(GL_LESS);                /* The Type Of Depth Test To Do */
    glEnable(GL_DEPTH_TEST);            /* Enables Depth Testing */
    glShadeModel(GL_SMOOTH);            /* Enables Smooth Color Shading */

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();                /* Reset The Projection Matrix */

    aspect = (GLdouble)Width / Height;
    glOrtho(-3.0, 3.0, -3.0 / aspect, 3.0 / aspect, 0.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
}

/* The main drawing function. */
void DrawGLScene(SDL_Window *window, GLuint texture, GLfloat * texcoord)
{
    /* Texture coordinate lookup, to make it simple */
    enum {
        MINX,
        MINY,
        MAXX,
        MAXY
    };

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);        /* Clear The Screen And The Depth Buffer */
    glLoadIdentity();                /* Reset The View */

    glTranslatef(-1.5f,0.0f,0.0f);        /* Move Left 1.5 Units */

    /* draw a triangle (in smooth coloring mode) */
    glBegin(GL_POLYGON);                /* start drawing a polygon */
    glColor3f(1.0f,0.0f,0.0f);            /* Set The Color To Red */
    glVertex3f( 0.0f, 1.0f, 0.0f);        /* Top */
    glColor3f(0.0f,1.0f,0.0f);            /* Set The Color To Green */
    glVertex3f( 1.0f,-1.0f, 0.0f);        /* Bottom Right */
    glColor3f(0.0f,0.0f,1.0f);            /* Set The Color To Blue */
    glVertex3f(-1.0f,-1.0f, 0.0f);        /* Bottom Left */
    glEnd();                    /* we're done with the polygon (smooth color interpolation) */

    glTranslatef(3.0f,0.0f,0.0f);         /* Move Right 3 Units */

    /* Enable blending */
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* draw a textured square (quadrilateral) */
/*glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor3f(1.0f,1.0f,1.0f);*/
    if (shaders_supported) {
        glUseProgramObjectARB(shaders[current_shader].program);
    }
    int num_verts = 250;
    GLuint line_vao = 0;
    GLuint line_vbo = 0;
    glGenVertexArray(&line_vao);
    glGenBuffer(&line_vbo);
    glBindVertexArray(line_vao);
    glBindBuffer(GL_ARRAY_BUFFER,line_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lineSegment)*250,&stuff[0],GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_LINES,0,250);
    if (shaders_supported) {
        glUseProgramObjectARB(0);
    }
   // glDisable(GL_TEXTURE_2D);

    /* swap buffers to display, since we're double buffered. */
    SDL_GL_SwapWindow(window);
}

int process_event(SDL_Event &event)
{
    if ( event.type == SDL_QUIT ) {
        return 1;
    }
    if ( event.type == SDL_KEYDOWN ) {
        if ( event.key.keysym.sym == SDLK_SPACE ) {
            return 2;
        }
        if ( event.key.keysym.sym == SDLK_ESCAPE ) {
            return 1;    
        }
    }
    return 0;
}

int check_events()
{
    SDL_Event event;
    while ( SDL_PollEvent(&event) ) {
        int result = process_event(event);
        if(result == 1)
            return result;
        if(result == 2)
            current_shader = (current_shader + 1) % NUM_SHADERS;
    }
    return 0;
}


static uint8_t *rtl_buffer;
static GLfloat *rtl_float_buffer;
struct lineSegment
{
	GLfloat x, y;
	GLfloat r, g, b;
};
static lineSegment stuff[250];
static uint32_t out_block_size = DEFAULT_BUF_LENGTH;

int init_sdr()
{
	int r, opt, i;
	int sync_mode = 0;
	uint8_t *buffer;
	int dev_index = 0;
	int count;
	int gains[100];

	rtl_buffer = malloc(out_block_size * sizeof(uint8_t));
	rtl_float_buffer = malloc(out_block_size * sizeof(GLfloat));

	dev_index = verbose_device_search("0");

	if (dev_index < 0) {
		return -1;
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		return -1;
	}
	
	verbose_set_sample_rate(dev, samp_rate);

	r = rtlsdr_set_testmode(dev, 1);

	verbose_reset_buffer(dev);
	return r;

}

static GLuint rtl_gl_buffer;
void bind_buffer_to_gl()
{
	glGenBuffers(1, &rtl_gl_buffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, rtl_gl_buffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLfloat)*out_block_size, rtl_float_buffer, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER,3,rtl_gl_buffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER,0);
//glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, 2, data_indices);
}

int rtl_read_buffer()
{
	int n_read;	
	int r = rtlsdr_read_sync(dev, rtl_buffer, out_block_size, &n_read);
	
	for(int i = 0; i<out_block_size; ++i)
	{
		rtl_float_buffer[i] = rtl_buffer[i];
	}
	for(int i = 0; i < 250; ++i)
	{
		stuff[i].x = i;
		stuff[i].y = rtl_buffer[i]/255.0f;
		stuff[i].r = 1.0f;
		stuff[i].g = 1.0f-stuff[i].y;
		stuff[i].b = 1.0f-stuff[i].x/250.0f;
	}
	/*glBindBuffer(GL_SHADER_STORAGE_BUFFER, rtl_gl_buffer);
	GLvoid* p = glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
	memcpy(p, rtl_float_buffer, sizeof(GLfloat)*out_block_size);
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER,0);*/
	return r;
}
int main(int argc, char **argv)
{
	int r = init_sdr();
	if(r == -1)
		return 1;
    int done;
    SDL_Window *window;
    SDL_Surface *surface;
    GLuint texture;
    GLfloat texcoords[4];

    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if ( SDL_Init(SDL_INIT_VIDEO) < 0 ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to initialize SDL: %s\n", SDL_GetError());
        exit(1);
    }

    window = SDL_CreateWindow( "RTL Shader DEMO", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL );
    if ( !window ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to create OpenGL window: %s\n", SDL_GetError());
        SDL_Quit();
        exit(2);
    }

    if ( !SDL_GL_CreateContext(window)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to create OpenGL context: %s\n", SDL_GetError());
        SDL_Quit();
        exit(2);
    }

    surface = SDL_LoadBMP("icon.bmp");
    if ( ! surface ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to load icon.bmp: %s\n", SDL_GetError());
        SDL_Quit();
        exit(3);
    }
    texture = SDL_GL_LoadTexture(surface, texcoords);
    SDL_FreeSurface(surface);
	GLenum err = glewInit();
    InitGL(640, 480);
    bind_buffer_to_gl(); 
    if (InitShaders()) {
        SDL_Log("Shaders supported, press SPACE to cycle them.\n");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Shaders not supported!\n");
    }
    done = 0;
    while ( ! done ) {
 	int r;	
	r = rtl_read_buffer();
        DrawGLScene(window, texture, texcoords);
        r = check_events();
        if(r == 1)
            done = 1;
    }
    QuitShaders();
    SDL_Quit();
    return 1;
}
