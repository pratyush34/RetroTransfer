#include <iostream>
#include <string>
#include <limits>
using namespace std;

void showMenu()
{
    cout << "\n=====================================\n";
    cout << "          RETRO TRANSFER\n";
    cout << "       Local File Transfer System\n";
    cout << "=====================================\n";
    cout << "1. Select Folder\n";
    cout << "2. Enter Receiver IP\n";
    cout << "3. Start Transfer\n";
    cout << "4. Exit\n";
    cout << "=====================================\n";
    cout << "Enter your choice: ";
}

void showDashboard(string folderPath, string receiverIP)
{
    cout << "\n========== CURRENT SETTINGS ==========\n";

    if (folderPath.empty())
        cout << "Folder      : Not selected\n";
    else
        cout << "Folder      : " << folderPath << endl;

    if (receiverIP.empty())
        cout << "Receiver IP : Not entered\n";
    else
        cout << "Receiver IP : " << receiverIP << endl;

    cout << "======================================\n";
}

int main()
{
    string folderPath;
    string receiverIP;
    int choice;

    while (true)
    {
        showMenu();

        if (!(cin >> choice))
        {
            cout << "\nInvalid input! Please enter a number.\n";
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            continue;
        }

        switch (choice)
        {
            case 1:
                cout << "\nEnter folder path: ";
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                getline(cin, folderPath);

                if (folderPath.empty())
                    cout << "Folder path cannot be empty.\n";
                else
                    cout << "Folder selected: " << folderPath << endl;
                break;

            case 2:
                cout << "\nEnter receiver IP address: ";
                cin >> receiverIP;

                cout << "Receiver IP saved: " << receiverIP << endl;
                break;

            case 3:
                showDashboard(folderPath, receiverIP);

                if (folderPath.empty())
                    cout << "Please select a folder first.\n";
                else if (receiverIP.empty())
                    cout << "Please enter receiver IP first.\n";
                else
                {
                    cout << "\nStatus: Ready for transfer.\n";
                    cout << "Actual file transfer will be added during integration.\n";
                }
                break;

            case 4:
                cout << "\nExiting RetroTransfer...\n";
                return 0;

            default:
                cout << "\nInvalid choice. Please select 1-4.\n";
        }
    }

    return 0;
}
