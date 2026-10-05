#include <iostream>
#include <string>
using namespace std;

void showMenu()
{
    cout << "\n=====================================\n";
    cout << "          RETRO TRANSFER\n";
    cout << "=====================================\n";
    cout << "1. Select Folder\n";
    cout << "2. Enter Receiver IP\n";
    cout << "3. Start Transfer\n";
    cout << "4. Exit\n";
    cout << "=====================================\n";
    cout << "Enter your choice: ";
}

int main()
{
    string folderPath;
    string receiverIP;

    int choice;

    while (true)
    {
        showMenu();
        cin >> choice;

        switch (choice)
        {
            case 1:
                cout << "\nEnter folder path: ";
                cin.ignore();
                getline(cin, folderPath);

                cout << "Folder selected: " << folderPath << endl;
                break;

            case 2:
                cout << "\nEnter receiver IP address: ";
                cin >> receiverIP;

                cout << "Receiver IP: " << receiverIP << endl;
                break;

            case 3:
                cout << "\nStarting transfer...\n";

                if (folderPath.empty())
                {
                    cout << "Please select a folder first.\n";
                }
                else if (receiverIP.empty())
                {
                    cout << "Please enter receiver IP first.\n";
                }
                else
                {
                    cout << "Folder : " << folderPath << endl;
                    cout << "Receiver: " << receiverIP << endl;
                    cout << "Transfer started!\n";
                }

                break;

            case 4:
                cout << "\nExiting RetroTransfer...\n";
                return 0;

            default:
                cout << "\nInvalid choice. Try again.\n";
        }
    }

    return 0;
}