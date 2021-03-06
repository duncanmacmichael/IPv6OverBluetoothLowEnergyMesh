﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.ServiceProcess;
using System.Text;
using System.Threading.Tasks;

namespace IPv6ToBlePacketProcessingForDesktop
{
    static class ServiceMain
    {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        static void Main()
        {
            ServiceBase[] ServicesToRun;
            ServicesToRun = new ServiceBase[]
            {
                new IPv6ToBlePacketProcessing()
            };
            ServiceBase.Run(ServicesToRun);
        }
    }
}
