﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Diagnostics;
using System.ComponentModel;
using System.Net;
using System.IO;

//
// Namespaces for driver interaction
//
using IPv6ToBleDriverInterfaceForDesktop;   // Driver interop library
using Microsoft.Win32.SafeHandles;          // Safe file handles
using System.Threading;                 // Asynchronous I/O and thread pool
using System.Runtime.InteropServices;   // Marhsalling interop calls

//
// Namespaces for Bluetooth
//
using IPv6ToBleBluetoothGattLibraryForDesktop;
using IPv6ToBleBluetoothGattLibraryForDesktop.Helpers;
using IPv6ToBleBluetoothGattLibraryForDesktop.Characteristics;
using IPv6ToBleAdvLibraryForDesktop;

//
// Other UWP namespaces
//
using Windows.Networking;
using Windows.Networking.Connectivity;

namespace IPv6ToBlePacketProcessingForDesktop
{
    /// <summary>
    /// The thread worker that handles the main logic for packet processing.
    /// This class is instantiated and stopped by the service's OnStart() and
    /// OnStop() methods respectively.
    /// </summary>
    public class ThreadWorker
    {
        #region Local variables
        //---------------------------------------------------------------------
        // Local variables
        //---------------------------------------------------------------------

        // The parent service for this worker thread
        private IPv6ToBlePacketProcessing parentService;

        // Getter and Setter for the parent service
        private IPv6ToBlePacketProcessing ParentService
        {
            get
            {
                return parentService;
            }

            set
            {
                if(parentService != value)
                {
                    parentService = value;
                }
            }
        }

        // A boolean to signal the worker thread to stop, if we're shutting
        // down or otherwise stopping the service
        private volatile bool shouldStop = false;

        // Booleans to track whether the packet has finished transmitting, and
        // whether it was successful
        private bool packetTransmissionFinished = false;
        private bool packetTransmittedSuccessfully = false;

        // Temporary hard-coded routing table
        private Dictionary<IPAddress, List<IPAddress>> staticRoutingTable;

        // This device's link-local IPv6 address
        private IPAddress localAddress = null;

        #endregion

        #region Constructor
        //---------------------------------------------------------------------
        // Constructor
        //---------------------------------------------------------------------

        public ThreadWorker(
            IPv6ToBlePacketProcessing parentService
        )
        {
            ParentService = parentService;            

            // TEMP: hard code the routing table. Each line is in the format:
            //
            // Node RouteToNodeFromBorderRouter
            staticRoutingTable = new Dictionary<IPAddress, List<IPAddress>>();

            // Parse the known device addresses
            bool parsed = false;
            IPAddress borderRouterAddress = null;
            IPAddress pi1Address = null;
            IPAddress pi2Address = null;

            // No error checking because these are known good addresses verified
            // here: http://v6decode.com/#address=fe80%3A%3A71c4%3A225%3Ad048%3A9476%2511
            // Also, constructors can't fail
            parsed = IPAddress.TryParse("fe80::b826:1c8b:ccbb:32f0%10", out borderRouterAddress);
            parsed = IPAddress.TryParse("fe80::291:a8ff:feeb:27b8", out pi1Address);
            parsed = IPAddress.TryParse("fe80::3ff8:d2ff:feeb:27b8", out pi2Address);

            // Test configuration is:
            // Border Router -> Pi 1 -> Pi 2
            //
            // Route 1: To Pi 1
            staticRoutingTable.Add(pi1Address, new List<IPAddress>());
            staticRoutingTable[pi1Address].Add(borderRouterAddress);
            staticRoutingTable[pi1Address].Add(pi1Address);

            // Route 2: To Pi 2
            staticRoutingTable.Add(pi2Address, new List<IPAddress>());
            staticRoutingTable[pi2Address].Add(borderRouterAddress);
            staticRoutingTable[pi2Address].Add(pi1Address);
            staticRoutingTable[pi2Address].Add(pi2Address);

        }
        #endregion

        #region Main business logic
        //---------------------------------------------------------------------
        // Main business logic for doing work and stopping
        //---------------------------------------------------------------------

        /// <summary>
        /// Method that is called when the thread is started. Prepares for 
        /// Driver and Bluetooth operations, then spins if it has nothing to do
        /// (i.e. we're waiting for an incoming packet). 
        /// 
        /// This is the main looping method of this thread.
        /// </summary>
        public void DoWork()
        {
            //
            // Step 1
            // On border router, acquire the local IPv6 address to use with
            // comparisons against packets' destination addresses
            //
            bool acquiredLocalV6Addr = IPAddress.TryParse(GetLocalIPv6Address(), out localAddress);
            if (!acquiredLocalV6Addr)
            {
                Debug.WriteLine("Could not acquire the local IPv6 address.");
                ParentService.ExitCode = -1;
                ParentService.Stop();
                throw new Exception();
            }
            // else acquire the generated address on a node within the subnet

            //
            // Step 2
            // Verify we can open a handle to the driver. If we can't, we fail
            // starting the service.
            //
            bool isDriverReachable = TryOpenHandleToDriver();
            if (!isDriverReachable)
            {
                Debug.WriteLine("Could not open a handle to the driver.");
                ParentService.ExitCode = -1;
                ParentService.Stop();
                throw new FileNotFoundException();
            }

            //
            // Step 3
            // Spin up the GATT server service to listen for later replies
            // over Bluetooth LE
            //
            IPv6ToBleGattServer gattServer = new IPv6ToBleGattServer();
            bool gattServerStarted = Task.Run(gattServer.StartAsync).Result;
            if (!gattServerStarted)
            {
                Debug.WriteLine("Could not start the GATT server.");
                ParentService.ExitCode = -1;
                ParentService.Stop();
                throw new Exception();
            }

            //
            // Step 4
            // Set up a Bluetooth advertiser to let neighbors know we can
            // receive a packet if need be. For testing, this is always on.
            //
            IPv6ToBleAdvPublisherPacketReceive packetReceiver = new IPv6ToBleAdvPublisherPacketReceive();
            packetReceiver.Start();

            //
            // Step 5
            // Indefinitely run in a cycle of sending listening requests to the
            // driver, waiting for a packet, receiving a packet from the driver,
            //sending a received packet over Bluetooth LE, etc.
            //
            while (!shouldStop)
            {
                SendListenRequestToDriverAndSendReceivedPacketAsync();
            }

            //
            // Step 6
            // Shut down Bluetooth resources upon exit
            //
            packetReceiver.Stop();            
        }

        // Method to signal the thread to stop
        public void RequestStop()
        {
            shouldStop = true;
        }
        #endregion

        #region Driver packet operations

        /// <summary>
        /// Verifies that the driver is present. Does not do anything else.
        /// </summary>
        /// <returns></returns>
        private bool TryOpenHandleToDriver()
        {
            bool isDriverReachable = true;

            SafeFileHandle driverHandle = IPv6ToBleDriverInterface.CreateFile(
                "\\\\.\\IPv6ToBle",
                IPv6ToBleDriverInterface.GENERIC_READ | IPv6ToBleDriverInterface.GENERIC_WRITE,
                IPv6ToBleDriverInterface.FILE_SHARE_READ | IPv6ToBleDriverInterface.FILE_SHARE_WRITE,
                IntPtr.Zero,
                IPv6ToBleDriverInterface.OPEN_EXISTING,
                0,
                IntPtr.Zero
            );

            if (driverHandle.IsInvalid)
            {
                int code = Marshal.GetLastWin32Error();

                Debug.WriteLine("Could not open a handle to the driver, " +
                                "error code: " + code.ToString()
                                );
                isDriverReachable = false;
            }

            return isDriverReachable;
        }

        /// <summary>
        /// Sends the IOCTL_IPV6_TO_BLE_LISTEN_NETWORK_V6 control code to the
        /// driver to ask the driver to listen for a packet.
        /// </summary>
        private unsafe void SendListenRequestToDriverAndSendReceivedPacketAsync()
        {
            //
            // Step 1
            // Open the handle to the driver with Overlapped async I/O flagged
            //
            SafeFileHandle driverHandle = IPv6ToBleDriverInterface.CreateFile(
                "\\\\.\\IPv6ToBle",
                IPv6ToBleDriverInterface.GENERIC_READ | IPv6ToBleDriverInterface.GENERIC_WRITE,
                IPv6ToBleDriverInterface.FILE_SHARE_READ | IPv6ToBleDriverInterface.FILE_SHARE_WRITE,
                IntPtr.Zero,
                IPv6ToBleDriverInterface.OPEN_EXISTING,
                IPv6ToBleDriverInterface.FILE_FLAG_OVERLAPPED,  // async
                IntPtr.Zero
            );

            if (driverHandle.IsInvalid)
            {
                int code = Marshal.GetLastWin32Error();

                Debug.WriteLine("Could not open a handle to the driver, " +
                                "error code: " + code.ToString()
                                );
                return;
            }

            //
            // Step 2
            // Prepare for asynchronous I/O
            //

            // Bind the driver handle to the Windows Thread Pool. Really, we're
            // binding the handle to an I/O completion port owned by the thread
            // pool.
            bool handleBound = ThreadPool.BindHandle(driverHandle);
            if (!handleBound)
            {
                Debug.WriteLine("Could not bind driver handle to a thread " +
                                "pool I/O completion port."
                                );
            }

            // Set up the byte array to hold a packet (always use 1280 bytes
            // to make sure we can receive the maximum Bluetooth packet size)
            byte[] packet = new byte[1280];
            int bytesReceived = 0;

            // Create a NativeOverlapped pointer to pass to DeviceIoControl().
            // The Pack() method packs the current managed Overlapped structure
            // into a native one, specifies a delegate callback for when the 
            // asynchronous operation completes, and specifies a managed object
            // (the packet) that serves as the buffer.
            Overlapped overlapped = new Overlapped();
            NativeOverlapped* nativeOverlapped = overlapped.Pack(IPv6ToBleListenCompletionCallback,
                                                                 packet
                                                                 );

            //
            // Step 3
            // Send the IOCTL to request the driver to listen for a packet and
            // give us the packet through the request's output buffer
            //
            // Note about asynchronous DeviceIoControl: when using overlapped
            // I/O, the call immediately returns with a boolean result. If
            // the result is true, the operation completed synchronously. If
            // false, it may be executing asynchronously. The error code is
            // ERROR_IO_PENDING for async I/O; otherwise it's a failure code.
            //
            // If DeviceIoControl() executes synchronously for some reason or
            // fails to execute asynchronously, the NativeOverlapped pointer
            // must still be freed.
            //            

            // System-defined ERROR_IO_PENDING == 997
            const int ErrorIoPending = 997;

            // Send the IOCTL
            bool listenResult = IPv6ToBleDriverInterface.DeviceIoControl(
                                    driverHandle,
                                    IPv6ToBleDriverInterface.IOCTL_IPV6_TO_BLE_LISTEN_NETWORK_V6,
                                    null,
                                    0,
                                    packet,
                                    (sizeof(byte) * 1280),
                                    out bytesReceived,
                                    nativeOverlapped
                                    );
            if (listenResult)
            {
                // Operation completed synchronously for some reason
                Debug.WriteLine("DeviceIoControl executed synchronously " +
                                "despite overlapped I/O flag."
                                );
                Overlapped.Unpack(nativeOverlapped);
                Overlapped.Free(nativeOverlapped);
            }
            else
            {
                int error = Marshal.GetLastWin32Error();

                if (error != ErrorIoPending)
                {
                    // Failed to execute DeviceIoControl with overlapped I/O
                    Debug.WriteLine("Failed to execute " +
                        "DeviceIoControl asynchronously with error code" +
                        error + "\n");
                    Overlapped.Unpack(nativeOverlapped);
                    Overlapped.Free(nativeOverlapped);
                }
                else
                {
                    // Wait for a packet to arrive. We do this by trying to get
                    // the overlapped result for 10 seconds, then aborting this
                    // attempt if it fails. This is so we can return control
                    // to the while loop in the DoWork() function, giving
                    // the thread a chance to see if it has been requested to
                    // stop.

                    listenResult = IPv6ToBleDriverInterface.GetOverlappedResultEx(
                                       driverHandle,
                                       nativeOverlapped,
                                       out bytesReceived,
                                       10000,  // 10 seconds
                                       false   // don't block forever
                                   );

                    // We have a packet. Send it out over Bluetooth. The very
                    // act of receiving a packet from the driver MUST mean that
                    // the packet was not meant for the border router, as the
                    // driver would have permitted that packet up the stack and
                    // not passed it here.
                    if(listenResult)
                    {
                        IPAddress destinationAddress = GetDestinationAddressFromPacket(packet);
                        SendPacketOverBluetoothLE(packet, destinationAddress);
                    }
                    else
                    {
                        // We waited 10 seconds for a packet from the driver
                        // and got nothing, so log the error, free the 
                        // NativeOverlapped structure, and continue
                        error = Marshal.GetLastWin32Error();
                        Debug.WriteLine("Did not receive a packet in the time" +
                                        " interval. Trying again. Error code: " +
                                        error
                                        );
                        Overlapped.Unpack(nativeOverlapped);
                        Overlapped.Free(nativeOverlapped);
                    }
                }
            }

            // Close the driver handle, implicitly canceling outstanding I/O
            IPv6ToBleDriverInterface.CloseHandle(driverHandle);
        }

        /// <summary>
        /// Delegate callback function for handling asynchronous I/O
        /// completion events.
        /// 
        /// This function cleans up the NativeOverlapped structure by 
        /// unpacking and freeing it.
        /// 
        /// For more info on this function, see
        /// https://msdn.microsoft.com/library/system.threading.iocompletioncallback
        /// </summary>
        private unsafe void IPv6ToBleListenCompletionCallback(
            uint errorCode,
            uint numBytes,
            NativeOverlapped* nativeOverlapped
        )
        {
            // Verify the operation succeeded, catch an exception if it did
            // not, and finally free the NativeOverlapped structure
            try
            {
                if(errorCode != 0)
                {
                    
                    throw new Win32Exception((int)errorCode);
                }
                else
                {
                    Overlapped.Unpack(nativeOverlapped);
                }
            } catch (Exception e)
            {
                Debug.WriteLine("Asynchronous Overlapped I/O failed with this" +
                                " error: " + e.Message
                                );
            }
            finally
            {
                Overlapped.Free(nativeOverlapped);
            }
        }

        #endregion

        #region Packet event listeners

        //---------------------------------------------------------------------
        // Listeners for when packets are transmitted or received
        //---------------------------------------------------------------------

        /// <summary>
        /// Awaits the asynchronous packet transmission operations by listening
        /// for the event from the Adv watcher that signals whether the packet
        /// transmitted successfully.
        /// 
        /// This is so a client can know when the packet has finished the
        /// transmission process.
        /// </summary>
        /// <param name="sender"></param>
        /// <param name="eventArgs"></param>
        /// <returns></returns>
        private void WatchForPacketTransmission(
            IPv6ToBleAdvWatcherPacketWrite sender,
            PropertyChangedEventArgs eventArgs
        )
        {
            if(eventArgs.PropertyName == "TransmittedSuccessfully")
            {
                packetTransmissionFinished = true;
                packetTransmittedSuccessfully = sender.TransmittedSuccessfully;
            }

        }

        /// <summary>
        /// Awaits the reception of a packet on the packet write characteristic
        /// of the packet write service of the local GATT server.
        /// 
        /// This is so a server can know when a packet has been received and
        /// deal with it accordingly.
        /// </summary>
        /// <param name="sender"></param>
        /// <param name="eventArgs"></param>
        private void WatchForPacketReception(
            IPv6ToBlePacketWriteCharacteristic localPacketWriteCharacteristic,
            PropertyChangedEventArgs eventArgs
        )
        {
            if(eventArgs.PropertyName == "Packet")
            {
                // Only send it back out if this device is not the destination;
                // in other words, if this device is a middle router in the
                // subnet
                IPAddress destinationAddress = GetDestinationAddressFromPacket(
                                                    localPacketWriteCharacteristic.Packet
                                                );
                if(IPAddress.Equals(destinationAddress, localAddress))
                {
                    SendPacketOverBluetoothLE(localPacketWriteCharacteristic.Packet, 
                                              destinationAddress
                                              );
                }

                
            }
        }
        #endregion

        #region Packet transmission

        /// <summary>
        /// Sends a packet out over Bluetooth Low Energy. Spins up a watcher
        /// for advertisements from nearby servers who can receive the packet.
        /// </summary>
        /// <param name="packet"></param>
        private void SendPacketOverBluetoothLE(
            byte[]      packet,
            IPAddress   destinationAddress
        )
        {
            // Fire up the Bluetooth Advertisement packet write
            // watcher
            IPv6ToBleAdvWatcherPacketWrite packetWriter = new IPv6ToBleAdvWatcherPacketWrite();

            // Start the watcher. This causes it to write the
            // packet when it finds a suitable recipient.
            packetWriter.Start(packet,
                               destinationAddress,
                               staticRoutingTable
                               );

            // Wait for the watcher to signal the event that the
            // packet has transmitted. This should happen after the
            // watcher receives an advertisement OR a timeout of
            // 5 seconds occurs.
            Stopwatch stopwatch = new Stopwatch();
            stopwatch.Start();
            while (!packetTransmissionFinished ||
                    stopwatch.Elapsed < TimeSpan.FromSeconds(5)
                    ) ;
            stopwatch.Stop();

            // Check transmission status
            if (!packetTransmittedSuccessfully)
            {
                Debug.WriteLine("Could not transmit the packet" +
                                " over Bluetooth LE successfully."
                                );
            }
            else
            {
                // We successfully transmitted the packet! Cue fireworks.
                Debug.WriteLine("Successfully transmitted this " +
                                "packet:\n" + Utilities.BytesToString(packet) +
                                "\n" + "to this address:\n" +
                                destinationAddress.ToString()
                                );
            }

            // Stop the watcher
            packetWriter.Stop();

            // Reset the packet booleans for next time
            packetTransmissionFinished = false;
            packetTransmittedSuccessfully = false;
        }

        #endregion

        #region Helpers

        /// <summary>
        /// Gets the local IPv6 address. This will retrieve the link-local
        /// IPv6 address from the FIRST device if there is more than one
        /// adapter installed. For most devices with one internet-connected
        /// adapter, this is fine. But if you switch, the address retrieved
        /// by this method will change depending on the adapter, so be careful
        /// with relying on knowing the address ahead of time.
        /// 
        /// This method is modified from the accepted answer on this post on 
        /// Stack Overflow:
        /// 
        /// https://stackoverflow.com/questions/11411486/how-to-get-ipv4-and-ipv6-address-of-local-machine
        /// 
        /// NOTE: This method is for the border router only, as the other
        /// devices in the subnet use their generated IPv6 address that is
        /// based on the Bluetooth radio ID.
        /// </summary>
        /// <returns></returns>
        private string GetLocalIPv6Address()
        {
            string hostNameString = Dns.GetHostName();
            IPHostEntry ipEntry = Dns.GetHostEntry(hostNameString);
            IPAddress[] addresses = ipEntry.AddressList;
            if (addresses[0].AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
            {
                return addresses[0].ToString(); // Can modify this array index if more than one adapter
            }
            else
            {
                return string.Empty;
            }
        }

        private IPAddress GetDestinationAddressFromPacket(byte[] packet)
        {
            // Get the destination IPv6 address from the packet.
            // The destination address is the last 16 bytes of the
            // 40-byte long IPv6 header, so it is bytes 23-39
            byte[] destinationAddressBytes = new byte[16];
            Array.ConstrainedCopy(packet,
                                  23,
                                  destinationAddressBytes,
                                  0,
                                  16
                                  );
            return new IPAddress(destinationAddressBytes);
        }

        #endregion
    }
}
