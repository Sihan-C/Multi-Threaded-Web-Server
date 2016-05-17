#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include "util.h"

#define MAX_THREADS 100
#define MAX_QUEUE_SIZE 100
#define MAX_REQUEST_LENGTH 1024

//Structure for queue.
typedef struct request_queue {
    int m_socket;
    char m_szRequest[ MAX_REQUEST_LENGTH ];
} request_queue_t;

int errno;
FILE *logfile;
int total_requests = 0;
int queue_len = 0, num_dispatchers = 0, num_workers = 0;
int bufin = 0, bufout = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logfile_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t items= PTHREAD_COND_INITIALIZER;
pthread_cond_t slots= PTHREAD_COND_INITIALIZER;
request_queue_t requestQ[ MAX_QUEUE_SIZE ];

/*
	SIGINT signal handler.
*/
void mysighand( int sigid ) {
	fclose( logfile );
	exit( 0 );
}

/*
	Writing error to log file.
*/
int writeError( int id, int handled_request, int fd, char *filename, char *errormsg ) {
	int error;
	if ( ( error = pthread_mutex_lock( &logfile_lock ) ) ) {
		fprintf( stderr, "Failed to acquire logfile lock: %s\n", strerror( error ) );
		return -1;
	}
    fprintf( logfile, "[%d][%d][%d][%s][%s]\n", id, handled_request, fd, filename, errormsg );
    fflush( logfile );
    if ( ( error = pthread_mutex_unlock( &logfile_lock ) ) ) {
		fprintf( stderr, "Failed to unlock logfile lock: %s\n", strerror( error ) );
		return -1;
	}
    return -1;
}

/* 
	Dispatcher thread
*/	
void *dispatch( void *arg ) {
    int fd, error;
    char filename[ MAX_REQUEST_LENGTH ];
    while ( 1 ) {
        if ( ( fd = accept_connection() ) < 0 )
            continue;
        if ( ( error = get_request( fd, filename ) != 0 ) )		// Ignore faulty request.
            continue; 
        
        // Adding request to request queue
        if ( ( error = pthread_mutex_lock( &queue_lock ) ) != 0 )
            continue; 
        while ( total_requests >= queue_len && !error )
            error = pthread_cond_wait( &slots, &queue_lock );
        requestQ[ bufin ].m_socket = fd;
        strcpy( requestQ[ bufin ].m_szRequest, filename );
        bufin = ( bufin + 1 ) % queue_len;	
        total_requests++;
        if ( ( error = pthread_cond_signal( &items ) ) ) {
            pthread_mutex_unlock( &queue_lock );
            continue;
        }
        if ( ( error = pthread_mutex_unlock( &queue_lock ) ) != 0 )
            continue;
    }
    return NULL;
}

/*	
	Worker thread
*/	
void *worker( void *arg ) {
    int error, fd;
    int handled_request = 0;
    int worker_id = *( int * )arg;
    char fname[ MAX_REQUEST_LENGTH ];
    char *filename;

    while( 1 ) {
        // Retrieving request from request queue
        if ( ( error = pthread_mutex_lock( &queue_lock ) ) != 0 )
            continue;
        while ( total_requests <= 0 && !error ) { 	
            error = pthread_cond_wait( &items, &queue_lock );
        }
        fd = requestQ[ bufout ].m_socket;
        strcpy( fname, requestQ[ bufout ].m_szRequest );

        // Remove slash '/'
        filename = fname;
        if ( fname[ 0 ] == '/' ) {
            filename = filename + 1;
        }
        bufout = ( bufout + 1 ) % queue_len;
        total_requests--;
        if ( ( error = pthread_cond_signal( &slots ) ) ) {
            pthread_mutex_unlock( &queue_lock );
        	continue;
        }
        if ( ( error = pthread_mutex_unlock( &queue_lock ) ) != 0 )
            continue;
            
        // Open the file
        int file_fd;
        if ( ( file_fd = open( filename, O_RDONLY ) ) == -1 ) {
        	perror( "Failed to open file" );
        	return_error( fd, strerror( errno ) );
        	writeError( worker_id, handled_request, fd, filename, strerror( errno ) );
            continue;
        }
        
        // Determine file size
        struct stat statbuf;
        off_t file_size;
        if ( stat( filename, &statbuf ) ) {
        	perror( "Failed to get file status" );
        	return_error( fd, strerror( errno ) );
        	writeError( worker_id, handled_request, fd, filename, strerror( errno ) );
            continue;
        }
        file_size = statbuf.st_size;

        // Read the file
        char *file_data = ( char * )malloc( file_size );	// Alocate memory to store file
        if ( read( file_fd, file_data, file_size ) == -1 ) {
        	perror( "Failed to read" );
        	return_error( fd, strerror( errno ) );
        	writeError( worker_id, handled_request, fd, filename, strerror( errno ) );
            continue;
        }

        // Determine content-type
        char content_type[ 15 ];
        char *last_four, *last_three;	// Last four and three characters of the request file name. 
        last_four = filename + ( strlen( filename ) - 4 );
        last_three = filename + ( strlen( filename ) - 3 );

        if ( strcmp( last_four, "html" ) == 0 || strcmp( last_three, "htm" ) == 0 ) {
        	strcpy( content_type, "text/html" );
        } else if ( strcmp( last_three, "jpg" ) == 0 ) {
        	strcpy( content_type, "image/jpeg" );
        } else if ( strcmp( last_three, "gif" ) == 0 ) {
        	strcpy( content_type, "image/gif" );
        } else {
        	strcpy( content_type, "text/plain" );
        }

        printf( "Return result to fd: %d\n",fd );
        if ( return_result( fd, content_type, file_data, file_size ) != 0 ) {
        	printf( "return result failed\n" );
            continue;
        }
        handled_request++;

        // Writing to log file
        if ( ( error = pthread_mutex_lock( &logfile_lock ) ) ) {
    		fprintf( stderr, "Failed to acquire logfile lock: %s\n", strerror( error ) );
    		continue;
    	}
        fprintf( logfile, "[%d][%d][%d][%s][%lld]\n", worker_id, handled_request, fd, filename, file_size );
        fflush( logfile );
        if ( ( error = pthread_mutex_unlock( &logfile_lock ) ) ) {
    		fprintf( stderr, "Failed to acquire logfile lock: %s\n", strerror( error ) );
    		continue;
    	}
        free( file_data );
    }
    return NULL;
}

int main( int argc, char **argv ) {
    //Error check first.
    if ( argc != 6 && argc != 7 ) {
        printf( "usage: %s port path num_dispatcher num_workers queue_length [cache_size]\n", argv[ 0 ] );
        return -1;
    }

    // Set signal handler for SIGINT
    struct sigaction newact;
    newact.sa_handler = mysighand;
    newact.sa_flags = 0;
    if ( ( sigfillset( &newact.sa_mask ) == -1 ) || ( sigaction( SIGINT, &newact, NULL ) == -1 ) ) { 
    	perror( "Failed to install SIGINT signal handler" );
        return -1;
    }

    // Create log file
    if ( ( logfile = fopen( "web_server_log", "a" ) ) == NULL ) {
    	perror( "Failed to create log file" );
    	return -1;
    }

    printf("Call init() first and make a dispatcher and worker threads\n");
    init( atoi( argv[ 1 ] ) );

    // Change directory to Web root
    if ( chdir( argv[ 2 ] ) == -1 ) {
        perror( "Change directory failed" );
        return -1;
    }
 
    // Handle input arguments
    num_dispatchers = atoi( argv[ 3 ] );
    num_workers = atoi( argv[ 4 ] );
    if ( num_dispatchers < 0 || num_dispatchers > MAX_THREADS ) {
        printf( "Invalid number of dispatcher. num_dispatcher should in the range [0,%d]\n", MAX_THREADS );
        return -1;    
    }
    if ( num_workers < 0 || num_workers > MAX_THREADS ) {
        printf( "Invalid number of workers. num_workers should in the range [0,%d]\n", MAX_THREADS );
        return -1;    
    }
    queue_len = atoi( argv[ 5 ] );
    if ( queue_len < 0 || queue_len > MAX_QUEUE_SIZE ) {
        printf( "Invalid queue size. queue_len should in the range [0,%d]\n", MAX_QUEUE_SIZE );
        return -1;
    }

    int i = 0, error;
    pthread_t dispatchers[ num_dispatchers ];
	pthread_t workers[ num_workers ];

    // Create dispatcher threads
    for ( i = 0; i < num_dispatchers; i++ ) {
        if ( ( error = pthread_create( &dispatchers[ i ], NULL, dispatch, NULL ) ) != 0 ) {
            fprintf( stderr, "Failed to create thread: %s\n", strerror( error ) );
            return -1;
        }        
    }

    // Create worker threads
    int *id_Arr[ num_workers ];
    for ( i = 0; i < num_workers; i++ ) {
    	// Pass thread id to worker to write to the logfile. 
    	id_Arr[ i ] = ( int * )malloc( sizeof( int ) );
    	*( id_Arr[ i ] ) = i;
        if ( ( error = pthread_create( &workers[ i ], NULL, worker, ( void * )( id_Arr[ i ] ) ) ) != 0 ) {
            fprintf( stderr, "Failed to create thread: %s\n", strerror( error ) );
            return -1;
        }       
    } 
    // Free memory allocated to pass id
    for ( i = 0; i < num_workers; i++ )
        free( id_Arr[ i ] );       

    // Join dispatcher threads
    for ( i = 0; i < num_dispatchers; i++ ) {
        if ( ( error = pthread_join( dispatchers[ i ], NULL ) ) != 0 ) {
            fprintf( stderr, "Failed to join thread: %s\n", strerror( error ) );
            return -1;
        }        
    }
    
    // Join worker threads
    for ( i = 0; i < num_workers; i++ ) {
        if ( ( error = pthread_join( workers[ i ], NULL ) ) != 0 ) {
            fprintf( stderr, "Failed to join thread: %s\n", strerror( error ) );
            return -1;
        }        
    }
    return 0;
}

