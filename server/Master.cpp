/* This file is part of ISAAC.
 *
 * ISAAC is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * ISAAC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Lesser Public
 * License along with ISAAC.  If not, see <www.gnu.org/licenses/>. */

#include "Master.hpp"
#include <stdio.h>
#include <pthread.h>
#include "ThreadList.hpp"
#include <sys/socket.h>
#include <string>
#if ISAAC_JPEG == 1
    #include <jpeglib.h>
#endif

volatile sig_atomic_t Master::force_exit = 0;

void sighandler(int sig)
{
	printf("\n");
	Master::force_exit = 1;
}

Master::Master(std::string name,int inner_port)
{
	this->name = name;
	this->inner_port = inner_port;
	this->insituThread = 0;
	masterHello = json_object();
	json_object_set_new( masterHello, "type", json_string( "hello" ) );
	json_object_set_new( masterHello, "name", json_string( name.c_str() ) );
}

errorCode Master::addDataConnector(MetaDataConnector *dataConnector)
{
	dataConnector->setMaster(this);
	MetaDataConnectorContainer d;
	d.connector = dataConnector;
	d.thread = 0;
	dataConnectorList.push_back(d);
	return 0;
}

errorCode Master::addImageConnector(ImageConnector *imageConnector)
{
	imageConnector->setMaster(this);
	ImageConnectorContainer d;
	d.connector = imageConnector;
	d.thread = 0;
	imageConnectorList.push_back(d);
	return 0;
}

MetaDataClient* Master::addDataClient()
{
	MetaDataClient* client = new MetaDataClient();
	dataClientList.push_back(client);
	client->masterSendMessage(new MessageContainer(MASTER_HELLO,masterHello,true));
	//Send all registered visualizations
	ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr mom = insituConnectorGroupList.getFront();
	while (mom)
	{
		if (mom->t->master && mom->t->video)
		{
			json_incref( mom->t->initData );
			client->masterSendMessage(new MessageContainer(REGISTER,mom->t->initData,true));
		}
		mom = mom->next;
	}
	return client;
}

void isaac_jpeg_init_source(j_decompress_ptr cinfo)
{
}
boolean isaac_jpeg_fill_input_buffer(j_decompress_ptr cinfo)
{
	return true;
}
void isaac_jpeg_skip_input_data(j_decompress_ptr cinfo,long num_bytes)
{
}
boolean isaac_jpeg_resync_to_restart(j_decompress_ptr cinfo, int desired)
{
	return true;
}
void isaac_jpeg_term_source(j_decompress_ptr cinfo)
{
}

size_t Master::receiveVideo(InsituConnectorGroup* group,uint8_t* video_buffer)
{
	char go = 42;
	send(group->video->connector->getSockFD(),&go,1,0);
	uint32_t count;
	recv(group->video->connector->getSockFD(),&count,4,0);
	bool jpeg = true;
	if (count == 0)
	{
		jpeg = false,
		count == group->video_buffer_size;
	}
	uint32_t received = 0;
	uint8_t* temp_buffer = video_buffer;
	if (jpeg)
		temp_buffer = (uint8_t*)malloc(count);
	while (received < count)
	{
		int r = recv(group->video->connector->getSockFD(),&(temp_buffer[received]),count - received,0);
		if (r <= 0)
			break;
		received += r;
	}
	if (jpeg)
	{
		#if ISAAC_JPEG == 1
			struct jpeg_decompress_struct cinfo;
			struct jpeg_error_mgr jerr;
			cinfo.err = jpeg_std_error(&jerr);
			jpeg_source_mgr src;
			src.init_source = &isaac_jpeg_init_source;
			src.fill_input_buffer = &isaac_jpeg_fill_input_buffer;
			src.skip_input_data = &isaac_jpeg_skip_input_data;
			src.resync_to_restart = &isaac_jpeg_resync_to_restart;
			src.term_source = &isaac_jpeg_term_source;
			jpeg_create_decompress(&cinfo);
			cinfo.src = &src;
			cinfo.src->next_input_byte = (JOCTET*)(temp_buffer);
			cinfo.src->bytes_in_buffer = count;
			(void) jpeg_read_header(&cinfo, TRUE);
			(void) jpeg_start_decompress(&cinfo);
			int row_stride = cinfo.output_width * cinfo.output_components;
			JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)
				((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);
			while (cinfo.output_scanline < cinfo.output_height)
			{
				int y = cinfo.output_scanline;
				(void) jpeg_read_scanlines(&cinfo, buffer, 1);
				for (int x = 0; x < cinfo.output_width; x++)
				{
					video_buffer[4*(x+y*cinfo.output_width)+0] = buffer[0][x*3+0];
					video_buffer[4*(x+y*cinfo.output_width)+1] = buffer[0][x*3+1];
					video_buffer[4*(x+y*cinfo.output_width)+2] = buffer[0][x*3+2];
				}
			}
			(void) jpeg_finish_decompress(&cinfo);
			jpeg_destroy_decompress(&cinfo);
			
		#else
			memset( video_buffer, rand()%255, group->video_buffer_size );
		#endif
		free(temp_buffer);
	}
	return received;
}

std::string Master::getStream(std::string connector,std::string name,std::string ref)
{
	void* reference = (void*)std::stol(ref);
	std::string result ("");
	ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr group = insituConnectorGroupList.getFront();
	while (group)
	{
		if (group->t->name == name)
		{
			pthread_mutex_lock(&group->t->streams_mutex);
			std::map< std::string, std::map< void* , std::string > >::iterator it = group->t->streams.find( connector );
			if (it != group->t->streams.end())
			{
				std::map< void* , std::string >::iterator it2 = (*it).second.find( reference );
				if (it2 != (*it).second.end())
					result = (*it2).second;
			}
			pthread_mutex_unlock(&group->t->streams_mutex);
		}
		group = group->next;
	}
	return result;
}

errorCode Master::run()
{
	printf("Running ISAAC Master\n");
	signal(SIGINT, sighandler);
	printf("Starting insitu plugin listener\n");
	if (insituMaster.init(inner_port))
	{
		fprintf(stderr,"Error while starting insitu plugin listener\n");
		signal(SIGINT, SIG_DFL);
		return -1;
	}
	pthread_create(&insituThread,NULL,Runable::run_runable,&insituMaster);
	
	for (std::list<MetaDataConnectorContainer>::iterator it = dataConnectorList.begin(); it != dataConnectorList.end(); it++)
	{
		printf("Launching %s\n",(*it).connector->getName().c_str());
		pthread_create(&((*it).thread),NULL,Runable::run_runable,(*it).connector);
	}
	for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
	{
		printf("Launching %s\n",(*it).connector->getName().c_str());
		pthread_create(&((*it).thread),NULL,Runable::run_runable,(*it).connector);
	}
	while (force_exit == 0)
	{
		/////////////////////////////////////
		// Iterate over all insitu clients //
		/////////////////////////////////////
		ThreadList< InsituConnectorContainer* >::ThreadListContainer_ptr insitu = insituMaster.insituConnectorList.getFront();
		while (insitu)
		{
			ThreadList< InsituConnectorContainer* >::ThreadListContainer_ptr next = insitu->next;
			//Check for new messages for every insituConnector
			MessageContainer* lastMessage = NULL;
			if (insitu->t->connector->messagesOut.back)
				lastMessage = insitu->t->connector->messagesOut.back->t;
			while (MessageContainer* message = insitu->t->connector->masterGetMessage())
			{
				bool delete_message = true;
				if (message->type == PERIOD)
				{
					if (insitu->t->group == NULL || insitu->t->group->video == NULL) //Later!
					{
						delete_message = false;
						insitu->t->connector->clientSendMessage( message );
					}
					else
					{
						//Let's see, whether some options are broadcastet and change them in the initData
						json_t* js;
						if (json_array_size( js = json_object_get(message->json_root, "projection") ) == 16)
							json_object_set( insitu->t->group->initData, "projection", js );
						if (json_array_size( js = json_object_get(message->json_root, "rotation") ) == 9)
							json_object_set( insitu->t->group->initData, "rotation", js );
						if (json_array_size( js = json_object_get(message->json_root, "position") ) == 3)
							json_object_set( insitu->t->group->initData, "position", js );
						if ( js = json_object_get(message->json_root, "distance") )
							json_object_set( insitu->t->group->initData, "distance", js );
						if ( js = json_object_get(message->json_root, "interpolation") )
							json_object_set( insitu->t->group->initData, "interpolation", js );
						//Allocate, receive and send video
						uint8_t*video_buffer=(uint8_t*)malloc(insitu->t->group->video_buffer_size);
						receiveVideo(insitu->t->group,video_buffer);
						//Send json data
						ThreadList<MetaDataClient*>::ThreadListContainer_ptr dc=dataClientList.getFront();
						while(dc)
						{
							if(dc->t->doesObserve(insitu->t->group->getID()))
								dc->t->masterSendMessage(new MessageContainer(message->type,message->json_root,true));
							dc=dc->next;
						}
						int l=imageConnectorList.size();
						ImageBufferContainer *container=new ImageBufferContainer(UPDATE_BUFFER,video_buffer,insitu->t->group,l);
						if(l==0)
							container->suicide();
						else
							for(std::list<ImageConnectorContainer>::iterator ic=imageConnectorList.begin();ic!=imageConnectorList.end();ic++)
								(*ic).connector->masterSendMessage(container);
					}
				}
				else
				if (message->type == REGISTER || message->type == REGISTER_VIDEO) //Saving the metadata description for later
				{
					//Get group
					std::string name( json_string_value( json_object_get( message->json_root, "name" ) ) );
					InsituConnectorGroup* group = NULL;
					ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr it = insituConnectorGroupList.getFront();
					while (it)
					{
						if (it->t->name == name)
						{
							group = it->t;
							break;
						}
						it = it->next;
					}
					if (group == NULL)
					{
						group = new InsituConnectorGroup(name);
						insituConnectorGroupList.push_back( group );
					}
					insitu->t->group = group;

					switch (message->type)
					{
						case REGISTER:
							group->initData = message->json_root;
							group->framebuffer_width = json_integer_value( json_object_get(group->initData, "framebuffer width") );
							group->framebuffer_height = json_integer_value( json_object_get(group->initData, "framebuffer height") );
							group->video_buffer_size = group->framebuffer_width*group->framebuffer_height*4;
							delete_message = false;
							printf("New connection, giving id %i (control)\n",insitu->t->connector->getID());
							break;
						case REGISTER_VIDEO:
							printf("New connection, giving id %i (video)\n",insitu->t->connector->getID());
							break;
					}
					
					if (message->type == REGISTER_VIDEO)
						group->video = insitu->t;
					else
					{
						group->master = insitu->t;
						group->id = insitu->t->connector->getID();
					}
					if (group->master && group->video)
					{
						printf("Group complete, sending to connected interfaces\n");
						ThreadList<MetaDataClient*>::ThreadListContainer_ptr dc = dataClientList.getFront();
						while (dc)
						{
							dc->t->masterSendMessage(new MessageContainer(REGISTER,group->initData,true));
							dc = dc->next;
						}
						for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
							(*it).connector->masterSendMessage(new ImageBufferContainer(GROUP_ADDED,NULL,group,1));
					}
				}
				else
				if (message->type == EXIT_PLUGIN) //Let's tell everybody and remove it from the list
				{
					InsituConnectorContainer* insituContainer = insitu->t;
					int id = insituContainer->connector->getID();
					printf("Connection %i closed.\n",id);
					//Group does still exist?
					if (insituContainer->group)
					{
						InsituConnectorGroup* group = insituContainer->group;
						//Add id of group
						json_object_set_new( message->json_root, "id", json_integer( group->getID() ) );
						ThreadList<MetaDataClient*>::ThreadListContainer_ptr dc = dataClientList.getFront();
						while (dc)
						{
							dc->t->masterSendMessage(new MessageContainer(EXIT_PLUGIN,message->json_root,true));
							dc = dc->next;
						}
						for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
							(*it).connector->masterSendMessage(new ImageBufferContainer(GROUP_FINISHED,NULL,group,1));
						//Now let's remove the whole group
						group->master->group = NULL;
						group->video->group = NULL;
						printf("Removed group %i\n",group->id);
						ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr it = insituConnectorGroupList.getFront();
						while (it)
						{
							if (it->t == group)
							{
								delete insituConnectorGroupList.remove(it);
								break;
							}
							it = it->next;
						}
					}
					delete insituMaster.insituConnectorList.remove(insitu);
					break;
				}
				if (delete_message)
					delete message;
				if (message == lastMessage)
					break;
			}
			insitu = next;
		}

		///////////////////////////////////////
		// Iterate over all metadata clients //
		///////////////////////////////////////
		ThreadList<MetaDataClient*>::ThreadListContainer_ptr dc = dataClientList.getFront();
		while (dc)
		{
			ThreadList<MetaDataClient*>::ThreadListContainer_ptr next = dc->next;
			//Check for new messages for every client
			while (MessageContainer* message = dc->t->masterGetMessage())
			{
				if (message->type == FEEDBACK)
				{
					json_t* observe_id = json_object_get(message->json_root, "observe id");
					if (observe_id)
					{
						int id = json_integer_value( observe_id );
						//Send feedback to observing insitu
						ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr group = insituConnectorGroupList.getFront();
						while (group)
						{
							char* buffer = json_dumps( message->json_root, 0 );
							if ( group->t->master->connector->getID() == id)
							{
								send(group->t->master->connector->getSockFD(),buffer,strlen(buffer),0);
								break;
							}
							free(buffer);
							group = group->next;
						}
					}
				}
				if (message->type == OBSERVE)
				{
					int id = json_integer_value( json_object_get(message->json_root, "observe id") );
					const char* url = json_string_value( json_object_get(message->json_root, "url") );
					void* ref = (void*)dc->t;
					dc->t->observe( id );
					InsituConnectorGroup* group = NULL;
					ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr it = insituConnectorGroupList.getFront();
					while (it)
					{
						if (it->t->getID() == id)
						{
							group = it->t;
							break;
						}
						it = it->next;
					}
					if (group)
					{
						json_t *js, *root = json_object();
						if (json_array_size( js = json_object_get( group->initData, "projection") ) == 16)
							json_object_set( root, "projection", js );
						if (json_array_size( js = json_object_get( group->initData, "rotation") ) == 9)
							json_object_set( root, "rotation", js );
						if (json_array_size( js = json_object_get( group->initData, "position") ) == 3)
							json_object_set( root, "position", js );
						if ( js = json_object_get( group->initData, "distance") )
							json_object_set( root, "distance", js );
						json_object_set_new( root, "type", json_string( "update" ) );
						dc->t->masterSendMessage(new MessageContainer(UPDATE,root));
						for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
							(*it).connector->masterSendMessage(new ImageBufferContainer(GROUP_OBSERVED,NULL,group,1,url,ref));
						//Send request for transfer functions
						char buffer[] = "{\"type\": \"feedback\", \"request\": \"transfer\"}";
						send(group->master->connector->getSockFD(),buffer,strlen(buffer),0);
					}
				}
				if (message->type == STOP)
				{
					int id = json_integer_value( json_object_get(message->json_root, "observe id") );
					const char* url = json_string_value( json_object_get(message->json_root, "url") );
					void* ref = (void*)dc->t;
					dc->t->stopObserve( id );
					InsituConnectorGroup* group = NULL;
					ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr it = insituConnectorGroupList.getFront();
					while (it)
					{
						if (it->t->getID() == id)
						{
							group = it->t;
							break;
						}
						it = it->next;
					}
					if (group)
						for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
							(*it).connector->masterSendMessage(new ImageBufferContainer(GROUP_OBSERVED_STOPPED,NULL,group,1,url,ref));
				}
				if (message->type == CLOSED)
				{
					void* ref = (void*)dc->t;
					ThreadList< InsituConnectorGroup* >::ThreadListContainer_ptr gr = insituConnectorGroupList.getFront();
					while (gr)
					{
						if (dc->t->doesObserve(gr->t->getID()))
						{
							for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
								(*it).connector->masterSendMessage(new ImageBufferContainer(GROUP_OBSERVED_STOPPED,NULL,gr->t,1,"",ref));
						}
						gr = gr->next;
					}
					dataClientList.remove(dc);
					break;
				}
				delete message;
			}
			dc = next;
		}

		///////////////////////////////////////
		// Iterate over all image connectors //
		///////////////////////////////////////
		for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
		{
			while (ImageBufferContainer* message = (*it).connector->masterGetMessage())
			{
				if (message->type == REGISTER_STREAM)
				{
					pthread_mutex_lock(&message->group->streams_mutex);
					message->group->streams[(*it).connector->getName()].insert( std::pair< void*,std::string >( message->reference, std::string((char*)message->buffer) ));
					pthread_mutex_unlock(&message->group->streams_mutex);
					json_t* root = json_object();
					json_object_set_new( root, "type", json_string ("register video") );
					json_object_set_new( root, "name", json_string ( message->group->getName().c_str() ) );
					json_object_set_new( root, "connector", json_string ( (*it).connector->getName().c_str() ) );
					json_object_set_new( root, "reference", json_integer ( (long)message->reference ) );
					ThreadList<MetaDataClient*>::ThreadListContainer_ptr dc = dataClientList.getFront();
					while (dc)
					{
						dc->t->masterSendMessage(new MessageContainer(REGISTER_VIDEO,root,true));
						dc = dc->next;
					}
					json_decref( root );
				}
				message->suicide();
			}
		}
		usleep(1);
	}


	//shutdown(insituMaster.getSockFD(),SHUT_RDWR);
	insituMaster.setExit();
	printf("Waiting for insitu Master thread to finish... ");
	fflush(stdout);
	//Yeah... "finish"
	pthread_join(insituThread,NULL);
	printf("Done\n");
	for (std::list<MetaDataConnectorContainer>::iterator it = dataConnectorList.begin(); it != dataConnectorList.end(); it++)
	{
		printf("Asking %s to exit\n",(*it).connector->getName().c_str());
		(*it).connector->masterSendMessage(new MessageContainer(FORCE_EXIT));
	}
	for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
	{
		printf("Asking %s to exit\n",(*it).connector->getName().c_str());
		(*it).connector->masterSendMessage(new ImageBufferContainer(IMG_FORCE_EXIT,NULL,NULL,1));
	}
	for (std::list<MetaDataConnectorContainer>::iterator it = dataConnectorList.begin(); it != dataConnectorList.end(); it++)
	{
		pthread_join((*it).thread,NULL);
		printf("%s finished\n",(*it).connector->getName().c_str());
	}
	for (std::list<ImageConnectorContainer>::iterator it = imageConnectorList.begin(); it != imageConnectorList.end(); it++)
	{
		pthread_join((*it).thread,NULL);
		printf("%s finished\n",(*it).connector->getName().c_str());
	}
	signal(SIGINT, SIG_DFL);
	return 0;
}

Master::~Master()
{
	json_decref( masterHello );
	dataConnectorList.clear();
	MetaDataClient* mom;
	while (mom = dataClientList.pop_front())
		delete mom;	
}
