
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

#define MAX_RADIO_RESOLUTION 1024
#define DEFAULT_SAMPLE_RATE		248000
#define DEFAULT_BUF_LENGTH		(1* MAX_RADIO_RESOLUTION)
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
static uint64_t curr_freq = 109000000;
static int32_t delta_freq = 0;

struct lineSegment
{
	GLfloat x, y;
};
static uint8_t *rtl_buffer;
#define MAX_TIME_IN_GRAPH 42
/* stuff works as a circular buffer */
static lineSegment stuff[MAX_TIME_IN_GRAPH][MAX_RADIO_RESOLUTION];
static int current_time = -1;
static bool roll_time = false;
static uint32_t out_block_size = DEFAULT_BUF_LENGTH;


/*****
 *   VISUAL CONTROLS  *
                  *****/

static float rotate_a = 15.0f;
static float rotate_b = 35.0f;
static float rotate_move_a = 0.2f;
static float rotate_move_b = 0.2f;
static float rotate_min = -65.0f;
static float rotate_max = 65.0f;

/*freq zoom*/
static float fzoom = 44.5f;
static float szoom = 0.5f;
static float dzoom = 0.5f;
static float mzoom = 47.2f;
static float mizoom = 15.0f;
/*depth zoom*/
static float zzoom = 1.0f;
static float szzoom = 0.02f;
static float minzzoom = 0.72f;
static float maxzzoom = 1.35f;

static GLfloat red_key = 1.0f;
static GLfloat blue_key = 1.0f;
static GLfloat green_key = 1.0f;

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



float frand()
{
	return static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
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
    glOrtho(-3.0, 3.0, -3.0 / aspect, 3.0 / aspect, -5.5, 5.5);

    glMatrixMode(GL_MODELVIEW);
}

void DrawGLScene(SDL_Window *window, GLuint texture, GLfloat * texcoord, float fzoom, float zzoom)
{

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);        /* Clear The Screen And The Depth Buffer */
	glLoadIdentity();                /* Reset The View */
	glRotatef(rotate_a, 1.0f, 0.0f, 0.0f);
	glRotatef(rotate_b, 0.0f, 1.0f, 0.0f);
	glTranslatef(-2.5f,0.0f,0.0f);        /* Move Left 1.5 Units */

	int time_to_render = roll_time? MAX_TIME_IN_GRAPH : current_time;
	int c_t = current_time;
	for(int t = 0; t<time_to_render;t++)
	{
		int start = 0 + fzoom*10.0;
		int end = 1023 - fzoom*10.0;
		glBegin(GL_LINES); 
		for(int i = start; i<end;++i)
		{
			GLfloat minus = (1.0*t)/(MAX_TIME_IN_GRAPH*1.0f);
			glColor4f( 1.0-(red_key *minus*1.6), 1.0 - (green_key*minus*1.6), 1.0-(blue_key*minus*1.6), 1.0-minus*0.85);
			GLfloat depth = 1.5-(1.0f*(t+0.01f)/(6.6f*zzoom));
			glVertex3f( -2.5f+((i-start)/((end-start)/10.0f)), -1.5f+stuff[c_t][i].y*3.0f, depth);
			glVertex3f( -2.5f+((i+1-start)/((end-start)/10.0f)), -1.5f+stuff[c_t][i+1].y*3.0f, depth);
		}
		glEnd();
		c_t = c_t - 1;
		if(c_t < 0)
			c_t = MAX_TIME_IN_GRAPH - 1;
	}
	SDL_GL_SwapWindow(window);
}


int init_sdr()
{
	int r, opt, i;
	int sync_mode = 0;
	uint8_t *buffer;
	int dev_index = 0;
	int count;
	int gains[100];

	rtl_buffer = malloc(out_block_size * sizeof(uint8_t));

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

	r = rtlsdr_set_testmode(dev, 0);

	rtlsdr_set_tuner_gain_mode(dev,0);
	rtlsdr_set_center_freq(dev,curr_freq);	
	rtlsdr_set_tuner_bandwidth(dev,22000);
	verbose_reset_buffer(dev);
	return r;

}

int circular_future_time()
{
	int future = current_time + 1;
	if(future >= MAX_TIME_IN_GRAPH)
	{
		future = 0;
		roll_time = true;
	}
	return future;
}

int rtl_read_buffer()
{
	int n_read;	

	int r = rtlsdr_read_sync(dev, rtl_buffer, out_block_size, &n_read);
	
	int future = circular_future_time();

	for(int i = 0; i < 1024; ++i)
	{
		stuff[future][i].x = i;
		stuff[future][i].y = ((float)rtl_buffer[i])/255.0f;
	}
	current_time = future;
	return r;
}



int process_event(SDL_Event &event)
{
    if ( event.type == SDL_QUIT ) {
        return 1;
    }
    if ( event.type == SDL_KEYDOWN && event.key.repeat == 0 ) {
        if ( event.key.keysym.sym == SDLK_SPACE ) {
            return 2;
        }
        if ( event.key.keysym.sym == SDLK_ESCAPE ) {
            return 1;    
        }
	switch (event.key.keysym.sym)
	{
		case SDLK_LEFT:
			delta_freq -= 10000;
			break;
		case SDLK_RIGHT:
			delta_freq += 10000;
			break;
		case SDLK_DOWN:
			delta_freq -= 1000;
			break;
		case SDLK_UP:
			delta_freq += 1000;
			break;
		case SDLK_COMMA:
			delta_freq -= 100000;
			break;
		case SDLK_PERIOD:
			delta_freq += 100000;
			break;
		default:
			break;
	}
    }else if( event.type == SDL_KEYUP)
    {
	switch (event.key.keysym.sym)
	{
		case SDLK_LEFT:
			delta_freq += 10000;
			break;
		case SDLK_RIGHT:
			delta_freq -= 10000;
			break;
		case SDLK_DOWN:
			delta_freq += 1000;
			break;
		case SDLK_UP:
			delta_freq -= 1000;
			break;
		case SDLK_COMMA:
			delta_freq += 100000;
			break;
		case SDLK_PERIOD:
			delta_freq -= 100000;
			break;
		default:
			break;
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
    }
}

void random_color_keys()
{
	float r = frand(); 
	if(r < 0.1)
	{
		red_key = frand();
		green_key = frand();
		blue_key = frand();
		while((red_key+green_key+blue_key) < 0.77)
		{
			red_key = frand();
			green_key = frand();
			blue_key = frand();
		}
	}
}

void random_rotation_control()
{
	if(frand() > 0.89)
		rotate_move_a = (0.5-frand())*2.0;
	if(frand() > 0.89)
		rotate_move_b = (0.5-frand())*2.0;

	rotate_a += rotate_move_a;
	rotate_b += rotate_move_b;

	if((rotate_a >= rotate_max) || (rotate_a <= rotate_min))
		rotate_move_a = - rotate_move_a;
	if((rotate_b >= rotate_max) || (rotate_b <= rotate_min))
		rotate_move_b = - rotate_move_b;
}

void random_zoom_control()
{
	fzoom += dzoom/(fzoom/mzoom);
	if(fzoom <=mizoom)
	{
		dzoom = frand()*szoom;
		fzoom = mizoom;
	}else if(fzoom >= mzoom)
	{
		dzoom = -szoom*frand();
		fzoom = mzoom;
	}
	zzoom += szzoom;
	if(zzoom>maxzzoom)
		szzoom = frand()*-0.021;
	else if(zzoom<minzzoom)
		szzoom = frand()*0.021;
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

	window = SDL_CreateWindow( "RTL DEMO", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1366, 768, SDL_WINDOW_OPENGL );
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

	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
	SDL_GL_SetSwapInterval(1);
	GLenum err = glewInit();
	InitGL(1366, 768);
	done = 0;
	
	SDL_GL_SetSwapInterval(1);
	while ( ! done ) {
		int r;
		r = check_events();
		if(r == 1)
			done = 1;
		r = rtl_read_buffer();

		random_color_keys();
		random_zoom_control();

		if(delta_freq != 0)
		{
			curr_freq += delta_freq;
			char cbufff[42];
			SDL_snprintf(cbufff,42,"Current frequency %d Hz\n", curr_freq);
			rtlsdr_set_center_freq(dev,curr_freq);	
			SDL_Log(cbufff);
		}
		DrawGLScene(window, texture, texcoords, fzoom, zzoom);
	}
}
