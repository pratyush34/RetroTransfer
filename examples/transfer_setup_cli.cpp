#include <iostream>
#include <string>
#include <limits>
#include <vector>
using namespace std;

void showMenu()
{
    cout << "\n";
    cout << "============================================\n";
    cout << "             RETRO TRANSFER\n";
    cout << "        Local File Transfer System v1.0\n";
    cout << "============================================\n";
    cout << "  1. Select Folder\n";
    cout << "  2. Configure Receiver IP\n";
    cout << "  3. Start Transfer\n";
    cout << "  4. View Dashboard\n";
    cout << "  5. Transfer History\n";
    cout << "  6. Exit\n";
    cout << "============================================\n";
    cout << "  Enter your choice: ";
}

void showDashboard(const string& folderPath,
                   const string& receiverIP)
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

void showHistory(const vector<string>& history)
{
    cout << "\n========== TRANSFER HISTORY ==========\n";

    if (history.empty())
    {
        cout << "No transfer requests in this session.\n";
    }
    else
    {
        for (size_t i = 0; i < history.size(); i++)
        {
            cout << i + 1 << ". " << history[i] << endl;
        }
    }

    cout << "======================================\n";
}

int main()
{
    string folderPath;
    string receiverIP;
    int choice;

    vector<string> transferHistory;

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
                    cout << "Folder selected: "
                         << folderPath << endl;

                break;

            case 2:
                cout << "\nEnter receiver IP address: ";
                cin >> receiverIP;

                cout << "Receiver IP saved: "
                     << receiverIP << endl;

                break;

            case 3:
            {
                showDashboard(folderPath, receiverIP);

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
                    char confirm;

                    cout << "\nReady to transfer the selected folder.\n";
                    cout << "Do you want to continue? (y/n): ";
                    cin >> confirm;

                    if (confirm == 'y' || confirm == 'Y')
                    {
                        transferHistory.push_back(
                            "Folder: " + folderPath +
                            " | Receiver: " + receiverIP +
                            " | Status: Requested (simulation)"
                        );

                        cout << "\nTransfer request recorded!\n";
                        cout << "Actual file transfer is not "
                             << "implemented yet.\n";
                    }
                    else
                    {
                        cout << "Transfer cancelled.\n";
                    }
                }

                break;
            }

            case 4:
                showDashboard(folderPath, receiverIP);
                break;

            case 5:
                showHistory(transferHistory);
                break;

            case 6:
                cout << "\nExiting Retro Transfer...\n";
                return 0;

            default:
                cout << "\nInvalid choice. Please select 1-6.\n";
        }
    }

    return 0;
}
