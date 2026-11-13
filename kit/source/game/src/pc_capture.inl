extern "C" const char *xbox_log_path(const char *path,char *out,size_t size);
static void capture_panel(ID3D11Device *device,ID3D11DeviceContext *context)
{
    static bool captured;
    static DWORD captured_at;
    static unsigned capture_index;
    const char *path=getenv("RECOMP_PC_UI_DUMP");
    unsigned interval=(unsigned)env_int("RECOMP_PC_UI_DUMP_INTERVAL_MS",0);
    bool always=env_int("RECOMP_PC_UI_DUMP_ALWAYS",0)!=0;
    if(!path || (!opened && !always) || (!always && GetTickCount()-opened_at<200) ||
       (captured && (!interval || GetTickCount()-captured_at<interval)))return;
    ID3D11RenderTargetView *view=nullptr;ID3D11Resource *resource=nullptr;
    ID3D11Texture2D *image=nullptr,*staging=nullptr;
    context->OMGetRenderTargets(1,&view,nullptr);if(!view)return;
    view->GetResource(&resource);view->Release();
    resource->QueryInterface(__uuidof(ID3D11Texture2D),(void **)&image);resource->Release();
    if(!image)return;
    D3D11_TEXTURE2D_DESC desc;image->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    if(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging))) {
        context->CopyResource(staging,image);D3D11_MAPPED_SUBRESOURCE mapped;
        if(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) {
            char numbered[MAX_PATH];
            if(interval){snprintf(numbered,sizeof(numbered),"%s-%04u.bmp",path,capture_index++);path=numbered;}
            FILE *file=fopen(path,"wb");
            if(file) {
                BITMAPFILEHEADER header={0};BITMAPINFOHEADER info={0};
                header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(info);
                header.bfSize=header.bfOffBits+desc.Width*desc.Height*4;
                info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=-(LONG)desc.Height;
                info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
                fwrite(&header,1,sizeof(header),file);fwrite(&info,1,sizeof(info),file);
                for(UINT y=0;y<desc.Height;y++)fwrite((char *)mapped.pData+y*mapped.RowPitch,4,desc.Width,file);
                fclose(file);captured=true;captured_at=GetTickCount();
                {char shown[MAX_PATH*2];fprintf(stderr,"[PC-MENU] captured native settings: %s\n",xbox_log_path(path,shown,sizeof shown));}
            }
            context->Unmap(staging,0);
        }
        staging->Release();
    }
    image->Release();
}
